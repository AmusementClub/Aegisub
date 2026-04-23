// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "subs_controller.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "ass_info.h"
#include "ass_style.h"
#include "app_runtime.h"
#include "compat.h"
#include "command/command.h"
#include "format.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "status_sink.h"
#include "subtitle_format.h"
#include "text_selection_controller.h"
#include "ui_services.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <wx/msgdlg.h>

namespace {
	void autosave_timer_changed(SubsControllerTimer *timer) {
		if (!timer)
			return;
		int freq = OPT_GET("App/Auto/Save Every Seconds")->GetInt();
		if (freq > 0 && OPT_GET("App/Auto/Save")->GetBool())
			timer->Start(freq * 1000);
		else
			timer->Stop();
	}

	int interaction_result_to_wx(agi::InteractionResult result) {
		switch (result) {
		case agi::InteractionResult::Ok:
			return wxOK;
		case agi::InteractionResult::Cancel:
			return wxCANCEL;
		case agi::InteractionResult::Yes:
			return wxYES;
		case agi::InteractionResult::No:
			return wxNO;
		}
		return wxCANCEL;
	}
}

struct SubsController::UndoInfo {
	std::string undo_description;
	int commit_id;

	std::vector<std::pair<std::string, std::string>> script_info;
	std::vector<AssStyle> styles;
	std::vector<AssDialogueBase> events;
	std::vector<AssAttachment> attachments;
	std::vector<ExtradataEntry> extradata;

	mutable std::vector<int> selection;
	int active_line_id = 0;
	int pos = 0, sel_start = 0, sel_end = 0;

	UndoInfo(const agi::Context *c, std::string const& d, int commit_id)
	: undo_description(d)
	, commit_id(commit_id)
	, attachments(c->GetCore().ass->Attachments)
	, extradata(c->GetCore().ass->Extradata)
	{
		auto core = c->GetCore();
		script_info.reserve(core.ass->Info.size());
		for (auto const& info : core.ass->Info)
			script_info.emplace_back(info.Key(), info.Value());

		styles.reserve(core.ass->Styles.size());
		styles.assign(core.ass->Styles.begin(), core.ass->Styles.end());

		events.reserve(core.ass->Events.size());
		events.assign(core.ass->Events.begin(), core.ass->Events.end());

		UpdateActiveLine(c);
		UpdateSelection(c);
		UpdateTextSelection(c);
	}

	void Apply(agi::Context *c) const {
		auto core = c->GetCore();
		// Keep old dialogue lines alive until after the commit is complete
		// since a bunch of stuff holds references to them
		AssFile old;
		old.Events.swap(core.ass->Events);
		core.ass->Info.clear();
		core.ass->Attachments.clear();
		core.ass->Styles.clear();
		core.ass->Extradata.clear();

		sort(begin(selection), end(selection));

		AssDialogue *active_line = nullptr;
		Selection new_sel;

		for (auto const& info : script_info)
			core.ass->Info.push_back(*new AssInfo(info.first, info.second));
		for (auto const& style : styles)
			core.ass->Styles.push_back(*new AssStyle(style));
		core.ass->Attachments = attachments;
		for (auto const& event : events) {
			auto copy = new AssDialogue(event);
			core.ass->Events.push_back(*copy);
			if (copy->Id == active_line_id)
				active_line = copy;
			if (binary_search(begin(selection), end(selection), copy->Id))
				new_sel.insert(copy);
		}
		core.ass->Extradata = extradata;

		core.ass->Commit("", AssFile::COMMIT_NEW);
		core.selectionController->SetSelectionAndActive(std::move(new_sel), active_line);

		core.textSelectionController->SetInsertionPoint(pos);
		core.textSelectionController->SetSelection(sel_start, sel_end);
	}

	void UpdateActiveLine(const agi::Context *c) {
		auto core = c->GetCore();
		auto line = core.selectionController->GetActiveLine();
		if (line)
			active_line_id = line->Id;
	}

	void UpdateSelection(const agi::Context *c) {
		auto core = c->GetCore();
		auto const& sel = core.selectionController->GetSelectedSet();
		selection.clear();
		selection.reserve(sel.size());
		for (const auto diag : sel)
			selection.push_back(diag->Id);
	}

	void UpdateTextSelection(const agi::Context *c) {
		auto core = c->GetCore();
		pos = core.textSelectionController->GetInsertionPoint();
		sel_start = core.textSelectionController->GetSelectionStart();
		sel_end = core.textSelectionController->GetSelectionEnd();
	}
};

SubsController::SubsController(agi::Context *context)
: context(context)
, undo_connection(context->GetCore().ass->AddUndoManager(&SubsController::OnCommit, this))
, text_selection_connection(context->GetCore().textSelectionController->AddSelectionListener(&SubsController::OnTextSelectionChanged, this))
, autosave_queue(agi::dispatch::Create())
{
	if (!IsGuiRuntimeShell())
		return;

	autosave_timer = CreateSubsControllerTimer([this] { AutoSave(); });
	autosave_timer_changed(autosave_timer.get());
	OPT_SUB("App/Auto/Save", [=] { autosave_timer_changed(autosave_timer.get()); });
	OPT_SUB("App/Auto/Save Every Seconds", [=] { autosave_timer_changed(autosave_timer.get()); });
}

SubsController::~SubsController() {
	// Make sure there are no autosaves in progress
	autosave_queue->Sync([]{ });
}

void SubsController::SetSelectionController(SelectionController *selection_controller) {
	auto core = context->GetCore();
	active_line_connection = core.selectionController->AddActiveLineListener(&SubsController::OnActiveLineChanged, this);
	selection_connection = core.selectionController->AddSelectionListener(&SubsController::OnSelectionChanged, this);
}

ProjectProperties SubsController::Load(agi::fs::path const& filename, std::string charset) {
	AssFile temp;
	auto core = context->GetCore();

	SubtitleFormat::GetReader(filename, charset)->ReadFile(&temp, filename, core.project->Timecodes(), charset, context->GetSingleChoiceInteractionSink(), core.backgroundRunnerFactory);

	core.ass->swap(temp);
	auto props = core.ass->Properties;

	SetFileName(filename);

	// Push the initial state of the file onto the undo stack
	undo_stack.clear();
	redo_stack.clear();
	autosaved_commit_id = saved_commit_id = commit_id + 1;
	core.ass->Commit("", AssFile::COMMIT_NEW);

	// Save backup of file
	if (CanSave() && OPT_GET("App/Auto/Backup")->GetBool()) {
		auto path_str = OPT_GET("Path/Auto/Backup")->GetString();
		agi::fs::path path;
		if (path_str.empty())
			path = filename.parent_path();
		else
			path = core.path->Decode(path_str);
		agi::fs::CreateDirectory(path);
		agi::fs::Copy(filename, path / agi::fs::PathFromString(agi::fs::PathToString(filename.stem()) + ".ORIGINAL" + agi::fs::PathToString(filename.extension())));
	}

	FileOpen(filename);
	return props;
}

void SubsController::Save(agi::fs::path const& filename, std::string const& encoding) {
	const SubtitleFormat *writer = SubtitleFormat::GetWriter(filename);
	if (!writer)
		throw agi::InvalidInputException("Unknown file type.");

	auto old_filename = this->filename;
	auto old_properties = context->GetCore().ass->Properties;
	int old_autosaved_commit_id = autosaved_commit_id, old_saved_commit_id = saved_commit_id;
	auto core = context->GetCore();
	try {
		autosaved_commit_id = saved_commit_id = commit_id;

		// Have to set this now for the sake of things that want to save paths
		// relative to the script in the header
		this->filename = filename;
		core.path->SetToken("?script", filename.parent_path());
		UpdateProperties();

		const AssFile *save_source = core.ass.get();
		std::unique_ptr<AssFile> save_copy;
		if (!core.ass->Extradata.empty()) {
			save_copy.reset(new AssFile(*core.ass));
			save_copy->CleanExtradata();
			save_source = save_copy.get();
		}

		writer->WriteFile(save_source, filename, core.project->Timecodes(), encoding, context->GetSingleChoiceInteractionSink());
		FileSave();
	}
	catch (...) {
		this->filename = old_filename;
		core.path->SetToken("?script", old_filename.parent_path());
		core.ass->Properties = std::move(old_properties);
		autosaved_commit_id = old_autosaved_commit_id;
		saved_commit_id = old_saved_commit_id;
		throw;
	}

	SetFileName(filename);
}

void SubsController::Close() {
	undo_stack.clear();
	redo_stack.clear();
	autosaved_commit_id = saved_commit_id = commit_id + 1;
	filename.clear();
	AssFile blank;
	auto core = context->GetCore();
	blank.swap(*core.ass);
	LoadDefaultAssFileWithAppOptions(*core.ass, true, OPT_GET("Subtitle Format/ASS/Default Style Catalog")->GetString());
	core.ass->Commit("", AssFile::COMMIT_NEW);
	FileOpen(filename);
}

int SubsController::TryToClose(bool allow_cancel) const {
	if (!IsModified())
		return wxYES;

	auto buttons = allow_cancel ? agi::InteractionButtons::YesNoCancel : agi::InteractionButtons::YesNo;
	int result = interaction_result_to_wx(context->RequestInteraction({
		from_wx(_("Unsaved changes")),
		from_wx(fmt_tl("Do you want to save changes to %s?", Filename())),
		buttons,
		agi::InteractionIcon::Question
	}));
	if (result == wxYES) {
		cmd::call("subtitle/save", context);
		// If it fails saving, return cancel anyway
		return IsModified() ? wxCANCEL : wxYES;
	}
	return result;
}

void SubsController::AutoSave() {
	if (commit_id == autosaved_commit_id)
		return;

	auto core = context->GetCore();
	auto directory = core.path->Decode(OPT_GET("Path/Auto/Save")->GetString());
	if (directory.empty())
		directory = filename.parent_path();

	auto name = filename.filename();
	if (name.empty())
		name = agi::fs::PathFromString("Untitled");

	autosaved_commit_id = commit_id;
	auto status_sink = context->GetStatusSink();
	auto choice_sink = context->GetSingleChoiceInteractionSink();
	auto subs_copy = new AssFile(*core.ass);
	auto fps = core.project->Timecodes();
	autosave_queue->Async([subs_copy, name, directory, status_sink, choice_sink, fps] {
		wxString msg;
		std::unique_ptr<AssFile> subs(subs_copy);

		try {
			agi::fs::CreateDirectory(directory);
			auto path = directory / agi::fs::PathFromString(agi::format("%s.%s.AUTOSAVE.ass",
				agi::fs::PathToString(name),
				agi::util::strftime("%Y-%m-%d-%H-%M-%S")));
			SubtitleFormat::GetWriter(path)->WriteFile(subs.get(), path, fps, "", choice_sink);
			msg = fmt_tl("File backup saved as \"%s\".", path);
		}
		catch (const agi::Exception& err) {
			msg = to_wx("Exception when attempting to autosave file: " + err.GetMessage());
		}
		catch (...) {
			msg = wxS("Unhandled exception when attempting to autosave file.");
		}

		if (status_sink)
			status_sink->ShowStatus(from_wx(msg));
	});
}

bool SubsController::CanSave() const {
	try {
		auto core = context->GetCore();
		return SubtitleFormat::GetWriter(filename)->CanSave(core.ass.get());
	}
	catch (...) {
		return false;
	}
}

void SubsController::SetFileName(agi::fs::path const& path) {
	filename = path;
	context->GetCore().path->SetToken("?script", path.parent_path());
	config::mru->Add("Subtitle", path);
	OPT_SET("Path/Last/Subtitles")->SetString(agi::fs::PathToString(filename.parent_path()));
}

void SubsController::OnCommit(AssFileCommit c) {
	if (c.message.empty() && !undo_stack.empty()) return;

	commit_id = next_commit_id++;
	// Allow coalescing only if it's the last change and the file has not been
	// saved since the last change
	if (commit_id == *c.commit_id+1 && redo_stack.empty() && saved_commit_id+1 != commit_id) {
		// If only one line changed just modify it instead of copying the file
		if (c.single_line && c.single_line->Group() == AssEntryGroup::DIALOGUE) {
			for (auto& diag : undo_stack.back().events) {
				if (diag.Id == c.single_line->Id) {
					diag = *c.single_line;
					break;
				}
			}
			*c.commit_id = commit_id;
			return;
		}

		undo_stack.pop_back();
	}

	// Make sure the file has at least one style and one dialogue line
	auto core = context->GetCore();
	if (core.ass->Styles.empty())
		core.ass->Styles.push_back(*new AssStyle);
	if (core.ass->Events.empty()) {
		core.ass->Events.push_back(*new AssDialogue);
		core.ass->Events.back().Row = 0;
	}

	redo_stack.clear();

	undo_stack.emplace_back(context, c.message, commit_id);

	int depth = std::max<int>(OPT_GET("Limits/Undo Levels")->GetInt(), 2);
	while ((int)undo_stack.size() > depth)
		undo_stack.pop_front();

	if (undo_stack.size() > 1 && OPT_GET("App/Auto/Save on Every Change")->GetBool() && !filename.empty() && CanSave())
		Save(filename);

	*c.commit_id = commit_id;
}

void SubsController::OnActiveLineChanged() {
	if (!undo_stack.empty())
		undo_stack.back().UpdateActiveLine(context);
}

void SubsController::OnSelectionChanged() {
	if (!undo_stack.empty())
		undo_stack.back().UpdateSelection(context);
}

void SubsController::OnTextSelectionChanged() {
	if (!undo_stack.empty())
		undo_stack.back().UpdateTextSelection(context);
}

void SubsController::Undo() {
	if (undo_stack.size() <= 1) return;
	redo_stack.splice(redo_stack.end(), undo_stack, std::prev(undo_stack.end()));

	commit_id = undo_stack.back().commit_id;

	text_selection_connection.Block();
	undo_stack.back().Apply(context);
	text_selection_connection.Unblock();
}

void SubsController::Redo() {
	if (redo_stack.empty()) return;
	undo_stack.splice(undo_stack.end(), redo_stack, std::prev(redo_stack.end()));

	commit_id = undo_stack.back().commit_id;

	text_selection_connection.Block();
	undo_stack.back().Apply(context);
	text_selection_connection.Unblock();
}

std::string SubsController::GetUndoDescription() const {
	return IsUndoStackEmpty() ? std::string() : undo_stack.back().undo_description;
}

std::string SubsController::GetRedoDescription() const {
	return IsRedoStackEmpty() ? std::string() : redo_stack.back().undo_description;
}

agi::fs::path SubsController::Filename() const {
	if (!filename.empty()) return filename;

	// Apple HIG says "untitled" should not be capitalised
#ifndef __WXMAC__
	return _("Untitled").wx_str();
#else
	return _("untitled").wx_str();
#endif
}
