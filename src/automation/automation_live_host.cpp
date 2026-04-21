// Copyright (c) 2026
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

#include "automation_live_host.h"

#include "../ass_attachment.h"
#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../ass_info.h"
#include "../ass_style.h"
#include "../async_video_provider.h"
#include "../audio_controller.h"
#include "../audio_timing.h"
#include "../auto4_base.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/context_ui.h"
#include "../options.h"
#include "../project.h"
#include "../subs_edit_box.h"
#include "../subs_controller.h"
#include "../ui_dispatch.h"
#include "../video_controller.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <utility>

namespace Automation4 {
namespace {

agi::ConstContextCoreSession GetCore(agi::Context const* context)
{
	return context->GetCore();
}

class LiveAutomationMediaState final : public AutomationMediaState {
	agi::Context const* context;

public:
	explicit LiveAutomationMediaState(agi::Context const* context)
	: context(context) {
	}

	AutomationMediaSnapshot CaptureSnapshot() const override
	{
		AutomationMediaSnapshot snapshot;
		if (!context)
			return snapshot;

		auto core = GetCore(context);
		snapshot.has_audio = core.project->AudioProvider() != nullptr;
		snapshot.has_timecodes = core.project->Timecodes().IsLoaded();
		snapshot.has_keyframes = !core.project->Keyframes().empty();
		snapshot.keyframes = core.project->Keyframes();

		if (auto video_info = TryGetVideoInfo()) {
			snapshot.has_video = true;
			snapshot.video_width = video_info->width;
			snapshot.video_height = video_info->height;
			snapshot.video_aspect_ratio = video_info->aspect_ratio;
			snapshot.video_aspect_ratio_type = video_info->aspect_ratio_type;
		}

		if (auto audio_selection = TryGetAudioSelection()) {
			snapshot.has_audio_selection = true;
			snapshot.audio_selection_begin = audio_selection->begin;
			snapshot.audio_selection_end = audio_selection->end;
		}

		return snapshot;
	}

	std::optional<int> FrameFromMs(int ms) const override
	{
		if (!context)
			return std::nullopt;

		auto core = GetCore(context);
		if (!core.project->Timecodes().IsLoaded())
			return std::nullopt;
		return core.videoController->FrameAtTime(ms, agi::vfr::START);
	}

	std::optional<int> MsFromFrame(int frame) const override
	{
		if (!context)
			return std::nullopt;

		auto core = GetCore(context);
		if (!core.project->Timecodes().IsLoaded())
			return std::nullopt;
		return core.videoController->TimeAtFrame(frame, agi::vfr::START);
	}

	std::optional<AutomationVideoInfo> TryGetVideoInfo() const override
	{
		if (!context)
			return std::nullopt;

		auto core = GetCore(context);
		auto provider = core.project->VideoProvider();
		if (!provider)
			return std::nullopt;

		return AutomationVideoInfo{
			provider->GetWidth(),
			provider->GetHeight(),
			core.videoController->GetAspectRatioValue(),
			static_cast<int>(core.videoController->GetAspectRatioType())
		};
	}

	std::vector<int> GetKeyframes() const override
	{
		if (!context)
			return {};
		return GetCore(context).project->Keyframes();
	}

	std::optional<AutomationAudioSelection> TryGetAudioSelection() const override
	{
		if (!context)
			return std::nullopt;

		auto core = GetCore(context);
		if (!core.audioController || !core.audioController->GetTimingController())
			return std::nullopt;

		TimeRange const range = core.audioController->GetTimingController()->GetActiveLineRange();
		return AutomationAudioSelection{ range.begin(), range.end() };
	}
};

class LiveAutomationUiProxy final : public AutomationUiProxy {
	agi::Context const* context;

public:
	explicit LiveAutomationUiProxy(agi::Context const* context)
	: context(context) {
	}

	void ShowStatus(std::string const& message, int timeout_ms) override
	{
		if (context)
			context->ShowStatus(message, timeout_ms);
	}

	bool SupportsInteractiveDialogs() const override
	{
		return context && context->GetUI().frame;
	}

	std::unique_ptr<BackgroundScriptRunner> CreateBackgroundScriptRunner(std::string const& title, AutomationUiAnchor anchor) const override
	{
		auto *parent = static_cast<wxWindow *>(anchor.native_parent);
		if (context) {
			if (auto runner = context->CreateAutomationBackgroundScriptRunner(title))
				return runner;
		}
		return std::make_unique<BackgroundScriptRunner>(parent, title, GetFileDialogService());
	}

	void ShowDialog(ProgressSink& sink, ScriptDialog& dialog) override
	{
		if (!SupportsInteractiveDialogs())
			throw AutomationError("interactive automation dialog unavailable in current host");
		sink.ShowDialog(&dialog);
	}

	std::vector<agi::fs::path> RequestOpenFiles(ProgressSink& sink, AutomationOpenFileDialogRequest const& request) override
	{
		return sink.RequestOpenFiles(request);
	}

	agi::fs::path RequestSaveFile(ProgressSink& sink, AutomationSaveFileDialogRequest const& request) override
	{
		return sink.RequestSaveFile(request);
	}

	std::shared_ptr<agi::FileDialogService> GetFileDialogService() const override
	{
		if (!SupportsInteractiveDialogs())
			return {};
		return context->GetFileDialogService();
	}

	bool CanFocusSubtitleEditBox() const override
	{
		return agi::ui::MainInvoke([this] {
			return context && context->GetUI().subsEditBox && context->GetUI().subsEditBox->CanFocusEditControl();
		});
	}

	bool FocusSubtitleEditBox() override
	{
		return agi::ui::MainInvoke([this] {
			if (!context)
				return false;
			auto ui = context->GetUI();
			if (!ui.subsEditBox || !ui.subsEditBox->CanFocusEditControl())
				return false;
			ui.subsEditBox->FocusEditControl();
			return true;
		});
	}

	bool SetSubtitleEditBoxCursor(int character_index, bool after) override
	{
		return agi::ui::MainInvoke([this, character_index, after] {
			if (!context)
				return false;
			auto ui = context->GetUI();
			if (!ui.subsEditBox || !ui.subsEditBox->CanFocusEditControl())
				return false;
			ui.subsEditBox->SetEditControlCaret(character_index, after);
			return true;
		});
	}
};

class LiveAutomationHost final : public AutomationHost {
	agi::Context const* context = nullptr;
	std::shared_ptr<AutomationMutationJournal> mutations;
	LiveAutomationMediaState media;
	LiveAutomationUiProxy ui;

public:
	LiveAutomationHost(agi::Context const* context, std::shared_ptr<AutomationMutationJournal> mutations)
	: context(context)
	, mutations(std::move(mutations))
	, media(context)
	, ui(context) {
	}

	bool HasProjectContext() const override
	{
		return context != nullptr;
	}

	void const* ProjectContextIdentity() const override
	{
		return context;
	}

	AutomationProjectSnapshot CaptureProjectSnapshot() const override
	{
		AutomationProjectSnapshot snapshot;
		if (!context)
			return snapshot;

		auto core = GetCore(context);
		if (!core.subsController->Filename().empty()) {
			snapshot.script_filename = agi::fs::PathToString(core.subsController->Filename().filename());
			snapshot.subtitle_file = agi::fs::PathToString(core.subsController->Filename());
		}
		snapshot.automation_scripts = core.ass->Properties.automation_scripts;
		snapshot.export_filters = core.ass->Properties.export_filters;
		snapshot.export_encoding = core.ass->Properties.export_encoding;
		snapshot.style_storage = core.ass->Properties.style_storage;
		snapshot.audio_file = agi::fs::PathToString(core.path->MakeAbsolute(core.ass->Properties.audio_file, "?script"));
		snapshot.video_file = agi::fs::PathToString(core.path->MakeAbsolute(core.ass->Properties.video_file, "?script"));
		snapshot.timecodes_file = agi::fs::PathToString(core.path->MakeAbsolute(core.ass->Properties.timecodes_file, "?script"));
		snapshot.keyframes_file = agi::fs::PathToString(core.path->MakeAbsolute(core.ass->Properties.keyframes_file, "?script"));
		snapshot.play_res_x = core.ass->GetScriptInfoAsInt("PlayResX");
		snapshot.play_res_y = core.ass->GetScriptInfoAsInt("PlayResY");
		snapshot.info_count = static_cast<int>(core.ass->Info.size());
		snapshot.style_count = static_cast<int>(core.ass->Styles.size());
		snapshot.event_count = static_cast<int>(core.ass->Events.size());
		snapshot.attachment_count = static_cast<int>(core.ass->Attachments.size());
		snapshot.extradata_count = static_cast<int>(core.ass->Extradata.size());
		for (auto const& event : core.ass->Events) {
			if (event.Comment)
				++snapshot.comment_count;
			else
				++snapshot.dialogue_count;
		}
		snapshot.video_zoom = core.ass->Properties.video_zoom;
		snapshot.ar_value = core.ass->Properties.ar_value;
		snapshot.scroll_position = core.ass->Properties.scroll_position;
		snapshot.ar_mode = core.ass->Properties.ar_mode;
		snapshot.active_row = core.ass->Properties.active_row;
		snapshot.video_position = core.ass->Properties.video_position;
		return snapshot;
	}

	AutomationMediaState const& Media() const override
	{
		return media;
	}

	AutomationUiProxy& Ui() override
	{
		return ui;
	}

	AutomationUiProxy const& Ui() const override
	{
		return ui;
	}

	std::optional<agi::fs::path> TryGetFileName() const override
	{
		if (!context)
			return std::nullopt;

		auto const filename = GetCore(context).subsController->Filename();
		if (filename.empty())
			return std::nullopt;
		return filename.filename();
	}

	agi::fs::path DecodePath(std::string const& path) const override
	{
		if (context)
			return GetCore(context).path->Decode(path);
		return config::path ? config::path->Decode(path) : agi::fs::PathFromString(path);
	}

	std::optional<AutomationProjectPropertiesView> TryGetProjectProperties() const override
	{
		if (!context)
			return std::nullopt;

		auto core = GetCore(context);
		AutomationProjectPropertiesView view;
		view.automation_scripts = core.ass->Properties.automation_scripts;
		view.export_filters = core.ass->Properties.export_filters;
		view.export_encoding = core.ass->Properties.export_encoding;
		view.style_storage = core.ass->Properties.style_storage;
		view.video_zoom = core.ass->Properties.video_zoom;
		view.ar_value = core.ass->Properties.ar_value;
		view.scroll_position = core.ass->Properties.scroll_position;
		view.active_row = core.ass->Properties.active_row;
		view.ar_mode = core.ass->Properties.ar_mode;
		view.video_position = core.ass->Properties.video_position;
		view.audio_file = core.path->MakeAbsolute(core.ass->Properties.audio_file, "?script");
		view.video_file = core.path->MakeAbsolute(core.ass->Properties.video_file, "?script");
		view.timecodes_file = core.path->MakeAbsolute(core.ass->Properties.timecodes_file, "?script");
		view.keyframes_file = core.path->MakeAbsolute(core.ass->Properties.keyframes_file, "?script");
		return view;
	}

	std::shared_ptr<AutomationMutationJournal> GetMutationJournal() const override
	{
		return mutations;
	}
};

}

std::shared_ptr<AutomationHost> CreateAutomationLiveHost(
	agi::Context const* context,
	std::shared_ptr<AutomationMutationJournal> mutations)
{
	return std::make_shared<LiveAutomationHost>(context, std::move(mutations));
}

}
