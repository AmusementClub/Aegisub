/// @file dialog_search_results.cpp
/// @see dialog_search_results.h

#include "dialog_search_results.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "text_selection_controller.h"
#include "utils.h"

#include <libaegisub/exception.h>

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <unordered_map>

namespace {
std::unordered_map<agi::Context *, DialogSearchResults *> search_results_dialogs;

void close_existing_results_dialog(agi::Context *context) {
	auto it = search_results_dialogs.find(context);
	if (it == search_results_dialogs.end())
		return;
	auto *dlg = it->second;
	search_results_dialogs.erase(it);
	dlg->Destroy();
}

std::string const& dialogue_field_text(AssDialogue const& line,
                                       SearchReplaceSettings::Field field) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return line.Text.get();
		case SearchReplaceSettings::Field::STYLE: return line.Style.get();
		case SearchReplaceSettings::Field::ACTOR: return line.Actor.get();
		case SearchReplaceSettings::Field::EFFECT: return line.Effect.get();
	}
	throw agi::InternalError("Bad field for search results");
}
}

void DialogSearchResults::InitCommon(bool replace_mode) {
	has_replacement = replace_mode;
	replacements_applied = replace_mode;

	hit_line_ids.clear();
	hit_line_ids.reserve(hits.size());
	for (auto const& hit : hits)
		hit_line_ids.insert(hit.line_id);

	list = new wxListView(this, -1, wxDefaultPosition, FromDIP(wxSize(860, 280)),
	                      wxLC_REPORT);

	// Column order: Line/Start/Style are quick orientation; Match|Replacement
	// pair the before/after fragments; Context|Replaced line pair the full
	// before/after line so a diff scan stays eye-adjacent.
	list->InsertColumn(0, _("Line"), wxLIST_FORMAT_RIGHT, FromDIP(50));
	list->InsertColumn(1, _("Start"), wxLIST_FORMAT_LEFT, FromDIP(90));
	list->InsertColumn(2, _("Style"), wxLIST_FORMAT_LEFT, FromDIP(100));
	list->InsertColumn(3, _("Match"), wxLIST_FORMAT_LEFT, FromDIP(120));
	if (has_replacement)
		list->InsertColumn(4, _("Replacement"), wxLIST_FORMAT_LEFT, FromDIP(120));
	list->InsertColumn(has_replacement ? 5 : 4, _("Context"),
	                   wxLIST_FORMAT_LEFT, FromDIP(240));
	if (has_replacement)
		list->InsertColumn(6, _("Replaced line"), wxLIST_FORMAT_LEFT, FromDIP(240));

	copy_button = new wxButton(this, -1, _("&Copy selected lines"));
	auto close_button = new wxButton(this, wxID_CLOSE);

	status = new wxStaticText(this, -1, wxEmptyString);

	auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(copy_button, wxSizerFlags().Border(wxRIGHT));
	button_sizer->AddStretchSpacer(1);
	button_sizer->Add(close_button);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(list, wxSizerFlags(1).Expand().Border());
	main_sizer->Add(status, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(button_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	SetSizerAndFit(main_sizer);
	CenterOnParent();

	list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &DialogSearchResults::OnActivate, this);
	copy_button->Bind(wxEVT_BUTTON, &DialogSearchResults::OnCopySelected, this);
	close_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

	// Modeless wxDialog defaults to Hide() on close; that would leave the
	// object and its AssFile commit listener alive forever. Match DialogManager:
	// Skip the event, then Destroy.
	Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& evt) {
		auto *dlg = static_cast<DialogSearchResults *>(evt.GetEventObject());
		evt.Skip();
		dlg->Destroy();
	});

	// Details give changed_lines so unrelated single-line edits do not wipe a
	// large find-all report.
	file_changed_slot = c->GetCore().ass->AddCommitDetailsListener(
		&DialogSearchResults::OnCommit, this);

	PopulateList();

	// Autosize text-heavy columns to their content so the new "Replaced line"
	// column is visible without horizontal scrolling. Fixed widths remain for
	// Line/Start/Style (short, bounded). Take the larger of content/header so a
	// long header label never truncates.
	auto fit = [&](int col) {
		list->SetColumnWidth(col, wxLIST_AUTOSIZE);
		int const content = list->GetColumnWidth(col);
		list->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
		int const header = list->GetColumnWidth(col);
		if (content > header)
			list->SetColumnWidth(col, content);
	};
	fit(3); // Match
	if (has_replacement) fit(4); // Replacement
	fit(has_replacement ? 5 : 4); // Context
	if (has_replacement) fit(6); // Replaced line
}

DialogSearchResults::DialogSearchResults(agi::Context *context,
                                         std::vector<aegisub::subtitle_match_report::MatchHit> matches)
: wxDialog(context->GetUI().parent, -1, _("Search results"),
           wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(context)
{
	hits.reserve(matches.size());
	for (auto& hit : matches) {
		DisplayHit d;
		d.line_id = hit.line_id;
		d.row = hit.row;
		d.start_time = std::move(hit.start_time);
		d.style = std::move(hit.style);
		d.matched = std::move(hit.matched);
		d.line_text = std::move(hit.line_text);
		d.field = hit.field;
		d.match_start = hit.start;
		d.match_end = hit.end;
		d.jump_start = hit.start;
		d.jump_end = hit.end;
		hits.push_back(std::move(d));
	}
	InitCommon(false);
}

DialogSearchResults::DialogSearchResults(agi::Context *context,
                                         std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements)
: wxDialog(context->GetUI().parent, -1, _("Replace results"),
           wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(context)
{
	hits.reserve(replacements.size());
	for (auto& hit : replacements) {
		DisplayHit d;
		d.line_id = hit.line_id;
		d.row = hit.row;
		d.start_time = std::move(hit.start_time);
		d.style = std::move(hit.style);
		d.matched = std::move(hit.matched);
		// Compute replaced_line before move()-ing line_text/replacement out of
		// `hit` — ReplacedLineText reads both.
		d.replaced_line = aegisub::subtitle_match_report::ReplacedLineText(hit);
		d.line_text = std::move(hit.line_text);
		d.replacement = std::move(hit.replacement);
		d.field = hit.field;
		d.match_start = hit.start;
		d.match_end = hit.end;
		// Text has already been rewritten; jump into the post-edit range until
		// an undo restores the pre-replacement field (see TryReconcile...).
		d.jump_start = hit.new_start;
		d.jump_end = hit.new_end;
		hits.push_back(std::move(d));
	}
	InitCommon(true);
}

DialogSearchResults::~DialogSearchResults() {
	auto it = search_results_dialogs.find(c);
	if (it != search_results_dialogs.end() && it->second == this)
		search_results_dialogs.erase(it);
}

void DialogSearchResults::PopulateList() {
	list->DeleteAllItems();

	for (std::size_t i = 0; i < hits.size(); ++i) {
		auto const& hit = hits[i];
		long idx = list->InsertItem(static_cast<long>(i), fmt_wx("%d", hit.row + 1));
		list->SetItem(idx, 1, to_wx(hit.start_time));
		list->SetItem(idx, 2, to_wx(hit.style));
		list->SetItem(idx, 3, to_wx(hit.matched));
		if (has_replacement)
			list->SetItem(idx, 4, to_wx(hit.replacement));
		list->SetItem(idx, has_replacement ? 5 : 4, to_wx(*hit.line_text));
		if (has_replacement)
			list->SetItem(idx, 6, to_wx(hit.replaced_line));
		list->SetItemData(idx, static_cast<long>(i));
	}

	if (stale)
		return;

	auto const n = static_cast<int>(hits.size());
	// Status follows whether replacements still apply, not whether the column
	// exists: after undo, jumps are pre-replacement ranges again.
	if (replacements_applied) {
		status->SetLabel(fmt_plural(n,
			"One match was replaced.",
			"%d matches were replaced.",
			n));
	}
	else {
		status->SetLabel(fmt_plural(n,
			"One match found.",
			"%d matches found.",
			n));
	}
}

void DialogSearchResults::MarkStale() {
	if (stale)
		return;
	stale = true;
	// Keep the list enabled so the user can still scroll through a large
	// outdated report; JumpToHit refuses navigation while stale.
	copy_button->Enable(false);
	// ASCII only inside _(): Aegisub is built with wxNO_IMPLICIT_WXSTRING_ENCODING,
	// so non-ASCII bytes in _("...") go through FromAscii and assert in debug.
	status->SetLabel(_("Results are out of date. Re-run Find All or Replace All."));
}

bool DialogSearchResults::TryReconcileAfterDocumentRebuild() {
	auto core = c->GetCore();

	for (auto& hit : hits) {
		AssDialogue *line = core.selectionController->GetDialogueById(hit.line_id);
		if (!line)
			return false;

		// Compare only the field that was searched. Matching any field would
		// falsely keep Style/Actor/Effect reports when Text happens to equal
		// the captured string.
		if (dialogue_field_text(*line, hit.field) != *hit.line_text)
			return false;

		hit.row = line->Row;
		hit.start_time = line->Start.GetAssFormatted();
		hit.style = line->Style.get();
		// Pre-replacement (or find-time) coordinates are valid again when the
		// field text matches line_text.
		hit.jump_start = hit.match_start;
		hit.jump_end = hit.match_end;
	}

	// Successful reconcile: field text is the captured original again, so any
	// replacements from this report are no longer applied.
	replacements_applied = false;
	stale = false;
	copy_button->Enable(true);
	PopulateList();
	return true;
}

void DialogSearchResults::OnCommit(AssFileCommitDetails commit) {
	// COMMIT_NEW is used by undo/redo (Ids preserved via AssDialogueBase) and
	// by load/close (new objects, dead Ids). Prefer reconcile over a blind stale.
	if (commit.type == AssFile::COMMIT_NEW) {
		if (!TryReconcileAfterDocumentRebuild())
			MarkStale();
		return;
	}

	// ADDREM / ORDER renumber rows and may drop lines; always invalidate.
	if ((commit.type & AssFile::COMMIT_DIAG_ADDREM) ||
	    (commit.type & AssFile::COMMIT_ORDER)) {
		MarkStale();
		return;
	}

	// Displayed columns include text, start time, and style; any of those on a
	// reported line can invalidate jump targets or the list contents.
	// COMMIT_STYLES alone is intentionally omitted: style manager delete/paste
	// can leave a dangling Style *name* in the column, but jump ranges are
	// unaffected, and greying the whole report on pure style-list edits is
	// worse UX. Style renames that also rewrite events include COMMIT_DIAG_META.
	static constexpr int kContentMask =
		AssFile::COMMIT_DIAG_TEXT |
		AssFile::COMMIT_DIAG_META |
		AssFile::COMMIT_DIAG_TIME;
	if (!(commit.type & kContentMask))
		return;

	// Many commits (multi-select edit, Replace Next/All) pass an empty
	// changed_lines span. Scope is then unknown: treat as whole-file dirty.
	if (commit.changed_lines.empty()) {
		MarkStale();
		return;
	}

	for (AssDialogue const *line : commit.changed_lines) {
		if (line && hit_line_ids.count(line->Id)) {
			MarkStale();
			return;
		}
	}
}

void DialogSearchResults::OnActivate(wxListEvent& evt) {
	// Item data is the hits index; visual row may diverge if columns are sorted.
	JumpToHit(static_cast<std::size_t>(list->GetItemData(evt.GetIndex())));
}

void DialogSearchResults::JumpToHit(std::size_t hit_index) {
	if (stale || hit_index >= hits.size())
		return;

	auto const& hit = hits[hit_index];
	auto core = c->GetCore();
	AssDialogue *line = core.selectionController->GetDialogueById(hit.line_id);
	if (!line)
		return;

	core.selectionController->SetSelectionAndActive({ line }, line);
	// Offsets are only meaningful in the subtitle text edit box for TEXT
	// searches (same rule as SearchReplaceEngine::FindReplace).
	if (hit.field == SearchReplaceSettings::Field::TEXT) {
		core.textSelectionController->SetSelection(static_cast<long>(hit.jump_start),
		                                            static_cast<long>(hit.jump_end));
	}
}

void DialogSearchResults::OnCopySelected(wxCommandEvent&) {
	if (stale)
		return;

	std::string out;
	long i = -1;
	while ((i = list->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
		auto const idx = static_cast<std::size_t>(list->GetItemData(i));
		if (idx >= hits.size())
			continue;
		if (!out.empty())
			out.push_back('\n');
		out += *hits[idx].line_text;
	}

	if (!out.empty())
		SetClipboard(out);
}

void DialogSearchResults::Show(agi::Context *context,
                               std::vector<aegisub::subtitle_match_report::MatchHit> matches) {
	if (matches.empty())
		return;
	close_existing_results_dialog(context);
	auto *dialog = new DialogSearchResults(context, std::move(matches));
	search_results_dialogs[context] = dialog;
	dialog->wxDialog::Show();
	dialog->Raise();
}

void DialogSearchResults::Show(agi::Context *context,
                               std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements) {
	if (replacements.empty())
		return;
	close_existing_results_dialog(context);
	auto *dialog = new DialogSearchResults(context, std::move(replacements));
	search_results_dialogs[context] = dialog;
	dialog->wxDialog::Show();
	dialog->Raise();
}

void DialogSearchResults::Dismiss(agi::Context *context) {
	close_existing_results_dialog(context);
}
