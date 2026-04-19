// Copyright (c) 2005, Rodrigo Braz Monteiro, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file frame_main.cpp
/// @brief Main window creation and control management
/// @ingroup main_ui

#include "frame_main.h"

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/menu.h"
#include "include/aegisub/toolbar.h"
#include "include/aegisub/hotkey.h"

#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "audio_box.h"
#include "base_grid.h"
#include "compat.h"
#include "command/command.h"
#include "dialog_detached_video.h"
#include "dialog_manager.h"
#include "libresrc/libresrc.h"
#include "main.h"
#include "options.h"
#include "project.h"
#include "perf_trace.h"
#include "status_sink.h"
#include "subs_controller.h"
#include "subs_edit_box.h"
#include "ui_services.h"
#include "utils.h"
#include "version.h"
#include "video_box.h"
#include "video_controller.h"
#include "video_display.h"
#include "wx_frame_main_dialog_ui_host.h"
#include "wx_frame_main_request_host.h"
#include "wx_frame_main_runtime_host.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <chrono>
#include <wx/dnd.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/sysopt.h>

#ifdef _WIN32
#include <dbt.h>
#include <windows.h>
#include <wtsapi32.h>
#endif

enum {
	ID_APP_TIMER_STATUSCLEAR = 12002
#ifdef _WIN32
	,ID_APP_TIMER_FONTCHANGE_DEBOUNCE
	,ID_APP_TIMER_AUDIO_OUTPUT_RECOVERY
#endif
};

#ifdef _WIN32
constexpr int kFontChangeDebounceDelayMs = 500;
constexpr int kAudioOutputRecoveryDebounceDelayMs = 750;

bool IsRelevantAudioDeviceChange(WXWPARAM wParam) {
	switch (static_cast<UINT>(wParam)) {
		case DBT_DEVNODES_CHANGED:
		case DBT_DEVICEARRIVAL:
		case DBT_DEVICEREMOVECOMPLETE:
			return true;
		default:
			return false;
	}
}

char const* DescribeAudioDeviceChange(WXWPARAM wParam) {
	switch (static_cast<UINT>(wParam)) {
		case DBT_DEVNODES_CHANGED: return "WM_DEVICECHANGE/DBT_DEVNODES_CHANGED";
		case DBT_DEVICEARRIVAL: return "WM_DEVICECHANGE/DBT_DEVICEARRIVAL";
		case DBT_DEVICEREMOVECOMPLETE: return "WM_DEVICECHANGE/DBT_DEVICEREMOVECOMPLETE";
		default: return "WM_DEVICECHANGE";
	}
}

bool IsRelevantSessionChange(WXWPARAM wParam) {
	switch (static_cast<DWORD>(wParam)) {
		case WTS_CONSOLE_CONNECT:
		case WTS_CONSOLE_DISCONNECT:
		case WTS_REMOTE_CONNECT:
		case WTS_REMOTE_DISCONNECT:
			return true;
		default:
			return false;
	}
}

char const* DescribeSessionChange(WXWPARAM wParam) {
	switch (static_cast<DWORD>(wParam)) {
		case WTS_CONSOLE_CONNECT: return "WM_WTSSESSION_CHANGE/WTS_CONSOLE_CONNECT";
		case WTS_CONSOLE_DISCONNECT: return "WM_WTSSESSION_CHANGE/WTS_CONSOLE_DISCONNECT";
		case WTS_REMOTE_CONNECT: return "WM_WTSSESSION_CHANGE/WTS_REMOTE_CONNECT";
		case WTS_REMOTE_DISCONNECT: return "WM_WTSSESSION_CHANGE/WTS_REMOTE_DISCONNECT";
		default: return "WM_WTSSESSION_CHANGE";
	}
}
#endif

#ifdef WITH_STARTUPLOG
#define StartupLog(a) agi::ShowFrameMainStartupLogDialog(wxS(a))
#else
#define StartupLog(a) LOG_I("frame_main/init") << a
#endif

FrameMain::FrameMain()
: wxFrame(nullptr, -1, wxEmptyString, wxDefaultPosition, wxSize(920,700), wxDEFAULT_FRAME_STYLE | wxCLIP_CHILDREN)
, context(agi::make_unique<agi::Context>())
{
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	SetSize(FromDIP(wxSize(920, 700)));
	StartupLog("Entering FrameMain constructor");

#ifdef __WXGTK__
	// XXX HACK XXX
	// We need to set LC_ALL to "" here for input methods to work reliably.
	setlocale(LC_ALL, "");

	// However LC_NUMERIC must be "C", otherwise some parsing fails.
	setlocale(LC_NUMERIC, "C");
#endif

	StartupLog("Initializing context controls");
	auto core = context->GetCore();
	auto ui = context->GetUI();
	ui_activation.AddConnections(
		core.ass->AddCommitListener(&FrameMain::UpdateTitle, this),
		core.subsController->AddFileOpenListener(&FrameMain::OnSubtitlesOpen, this),
		core.subsController->AddFileSaveListener(&FrameMain::UpdateTitle, this),
		core.project->AddAudioProviderListener(&FrameMain::OnAudioOpen, this),
		core.project->AddVideoProviderListener(&FrameMain::OnVideoOpen, this));
	observe_phase("startup.frame.context.bind_core_listeners");

	StartupLog("Initializing context frames");
	ui.parent = this;
	ui.frame = this;
	core.statusSink = agi::MakeFrameMainStatusSink(
		[this](std::string const& message, int timeout_ms) {
			StatusTimeout(to_wx(message), timeout_ms);
		},
		GetAsyncUiLifetime());
	core.notificationSink = agi::MakeFrameMainNotificationSink(this, GetAsyncUiLifetime());
	core.interactionSink = agi::MakeFrameMainInteractionSink(this, GetAsyncUiLifetime());
	core.singleChoiceInteractionSink = agi::MakeFrameMainSingleChoiceInteractionSink(this, GetAsyncUiLifetime());
	core.fileDialogService = agi::MakeFrameMainFileDialogService(this, GetAsyncUiLifetime());
	core.videoSourceRequestService = agi::MakeFrameMainVideoSourceRequestService(this, GetAsyncUiLifetime());
	core.backgroundRunnerFactory = agi::MakeFrameMainBackgroundRunnerFactory(this, GetAsyncUiLifetime());
	core.projectUiStateSink = agi::MakeFrameMainProjectUiStateSink(
		[context = context.get()](agi::ProjectUiStateSnapshot const& state) {
			auto ui = context->GetUI();
			if (state.subtitle_scroll_position && ui.subsGrid)
				ui.subsGrid->ScrollTo(*state.subtitle_scroll_position);
			if (state.video_zoom && ui.videoDisplay)
				ui.videoDisplay->SetZoom(*state.video_zoom);
		},
		GetAsyncUiLifetime());
	core.audioPlayerFactoryService = agi::MakeFrameMainAudioPlayerFactoryService(this, GetAsyncUiLifetime());
	core.automationBackgroundScriptRunnerFactory = agi::MakeFrameMainAutomationBackgroundScriptRunnerFactory(this, GetAsyncUiLifetime());
	observe_phase("startup.frame.context.install_ui_services");

	StartupLog("Set frame background for resize painting");
	SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE));

	StartupLog("Apply saved Maximized state");
	if (OPT_GET("App/Maximized")->GetBool()) Maximize(true);

	StartupLog("Initialize toolbar");
	wxSystemOptions::SetOption(wxS("msw.remap"), 0);
	OPT_SUB("App/Show Toolbar", &FrameMain::EnableToolBar, this);
	EnableToolBar(*OPT_GET("App/Show Toolbar"));
	observe_phase("startup.frame.toolbar.attach_or_hide");

	StartupLog("Initialize menu bar");
	menu::GetMenuBar("main", this, (wxID_HIGHEST + 1) + 10000, context.get());
	observe_phase("startup.frame.menu.attach");

	StartupLog("Create status bar");
	CreateStatusBar(2);

	StartupLog("Set icon");
#ifdef _WIN32
	SetIcon(wxICON(wxicon));
#else
	wxIcon icon;
	icon.CopyFromBitmap(GETIMAGE(wxicon));
	SetIcon(icon);
#endif

	StartupLog("Create views and inner main window controls");
	InitContents();
	OPT_SUB("Video/Detached/Enabled", &FrameMain::OnVideoDetach, this);
	observe_phase("startup.frame.contents.total");

	StartupLog("Set up drag/drop target");
	SetDropTarget(agi::MakeFrameMainFileDropTarget(
		[this](std::vector<agi::fs::path> const& files) {
			auto *ctx = context.get();
			if (!ctx)
				return;

			auto core = ctx->GetCore();
			if (!OPT_GET("Video/Secondary Subtitles/Enabled")->GetBool()) {
				core.project->LoadList(files);
				return;
			}

			auto is_subtitle_drop_file = [](agi::fs::path const& path) {
				// Match the subtitle list in Project::LoadList. Avoid container
				// formats like mkv which can be both video and subtitles.
				return agi::fs::HasExtension(path, "ass")
					|| agi::fs::HasExtension(path, "ssa")
					|| agi::fs::HasExtension(path, "srt")
					|| agi::fs::HasExtension(path, "sub")
					|| agi::fs::HasExtension(path, "ttxt");
			};

			std::vector<agi::fs::path> subtitle_files;
			std::vector<agi::fs::path> other_files;
			subtitle_files.reserve(files.size());
			other_files.reserve(files.size());
			for (auto const& file : files) {
				if (is_subtitle_drop_file(file))
					subtitle_files.push_back(file);
				else
					other_files.push_back(file);
			}

			if (subtitle_files.empty()) {
				core.project->LoadList(files);
				return;
			}

			agi::SingleChoiceInteractionRequest request;
			request.title = from_wx(_("Dropped subtitles"));
			request.message = from_wx(_("Where do you want to load the dropped subtitle file?"));
			request.choices = {
				from_wx(_("Main subtitles")),
				from_wx(_("Secondary subtitles"))
			};
			request.default_choice = 0;
			request.request_id = "frame_main.drop_target.subtitle_destination";

			auto choice = ctx->RequestSingleChoice(request);
			if (!choice) {
				// Cancel: still load non-subtitle files (e.g. video), but skip the subtitles.
				if (!other_files.empty())
					core.project->LoadList(other_files);
				return;
			}

			if (*choice == 0) {
				core.project->LoadList(files);
				return;
			}

			if (!other_files.empty())
				core.project->LoadList(other_files);

			if (videoBox)
				videoBox->OpenSecondarySubtitlesFromPath(subtitle_files.front());

			if (subtitle_files.size() > 1) {
				ctx->ShowInfo(
					from_wx(_("Multiple subtitle files were dropped. Only the first one was loaded as secondary.")),
					from_wx(_("Secondary subtitles")));
			}
		},
		GetAsyncUiLifetime()));
	observe_phase("startup.frame.drag_drop.install");

	StartupLog("Load default file");
	core.project->CloseSubtitles();
	observe_phase("startup.frame.project.close_initial_subtitles");

	StartupLog("Display main window");
	AddFullScreenButton(this);
	Show();
	SetDisplayMode(1, 1);
#ifdef _WIN32
	RegisterSessionNotifications();
#endif
	observe_phase("startup.frame.show");

	StartupLog("Leaving FrameMain constructor");
}

FrameMain::~FrameMain () {
	ui_activation.Deactivate();
	auto core = context->GetCore();
#ifdef _WIN32
	FontChangeDebounce.Stop();
	AudioOutputRecoveryDebounce.Stop();
	UnregisterSessionNotifications();
#endif
	core.project->CloseAudio();
	core.project->CloseVideo();

	DestroyChildren();
}

void FrameMain::EnableToolBar(agi::OptionValue const& opt) {
	if (opt.GetBool()) {
		if (!GetToolBar()) {
			toolbar::AttachToolbar(this, "main", context.get(), "Default");
			GetToolBar()->Realize();
		}
	}
	else if (wxToolBar *old_tb = GetToolBar()) {
		SetToolBar(nullptr);
		delete old_tb;
		Layout();
	}
}

void FrameMain::InitContents() {
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	StartupLog("Create background panel");
	contentsPanel = new wxPanel(this, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);

	StartupLog("Create subtitles grid");
	auto ui = context->GetUI();
	ui.subsGrid = new BaseGrid(contentsPanel, context.get());

	StartupLog("Create subtitle editing box");
	auto EditBox = new SubsEditBox(contentsPanel, context.get());
	observe_phase("startup.frame.contents.create_base_controls");

	StartupLog("Arrange main sizers");
	ToolsSizer = new wxBoxSizer(wxVERTICAL);
	ToolsSizer->Add(EditBox, 1, wxEXPAND);
	TopSizer = new wxBoxSizer(wxHORIZONTAL);
	TopSizer->Add(ToolsSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
	MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(new wxStaticLine(contentsPanel),0,wxEXPAND | wxALL,0);
	MainSizer->Add(TopSizer,0,wxEXPAND | wxALL,0);
	MainSizer->Add(ui.subsGrid,1,wxEXPAND | wxALL,0);
	contentsPanel->SetSizer(MainSizer);
	observe_phase("startup.frame.contents.create_sizers");

	StartupLog("Perform layout");
	Layout();
	observe_phase("startup.frame.contents.initial_layout");
	StartupLog("Leaving InitContents");
}

void FrameMain::EnsureVideoBoxCreated() {
	if (videoBox)
		return;

	bool didFreeze = contentsPanel && !contentsPanel->IsFrozen();
	if (didFreeze)
		contentsPanel->Freeze();

	videoBox = new VideoBox(contentsPanel, false, context.get());
	videoBox->Hide();
	TopSizer->Insert(0, videoBox, 0, wxEXPAND, 0);
	TopSizer->Show(videoBox, false, true);
	videoBox->SyncToContextState();

	if (didFreeze)
		contentsPanel->Thaw();
}

void FrameMain::EnsureAudioBoxCreated() {
	if (audioBox)
		return;

	bool didFreeze = contentsPanel && !contentsPanel->IsFrozen();
	if (didFreeze)
		contentsPanel->Freeze();

	auto ui = context->GetUI();
	ui.audioBox = audioBox = new AudioBox(contentsPanel, context.get());
	ToolsSizer->Insert(0, audioBox, 0, wxEXPAND);
	ToolsSizer->Show(audioBox, false, true);
	audioBox->SyncToContextState();

	if (didFreeze)
		contentsPanel->Thaw();
}

void FrameMain::SyncAudioOpenUi() {
	pending_audio_open_ui_sync = false;
	if (IsBeingDeleted())
		return;

	auto core = context->GetCore();
	if (!core.project->AudioProvider()) {
		SetDisplayMode(-1, 0);
		return;
	}

	EnsureAudioBoxCreated();
	SetDisplayMode(-1, 1);
}

void FrameMain::SyncVideoOpenUi() {
	pending_video_open_ui_sync = false;
	if (IsBeingDeleted())
		return;

	auto core = context->GetCore();
	auto provider = core.project->VideoProvider();
	if (!provider) {
		SetDisplayMode(0, -1);
		return;
	}

	Freeze();
	EnsureVideoBoxCreated();
	int vidx = provider->GetWidth(), vidy = provider->GetHeight();
	auto ui = context->GetUI();

	double zoom = ui.videoDisplay->GetZoom();
	wxSize windowSize = GetSize();
	if (vidx*3*zoom > windowSize.GetX()*4 || vidy*4*zoom > windowSize.GetY()*6)
		ui.videoDisplay->SetZoom(zoom * .25);
	else if (vidx*3*zoom > windowSize.GetX()*2 || vidy*4*zoom > windowSize.GetY()*3)
		ui.videoDisplay->SetZoom(zoom * .5);

	SetDisplayMode(1,-1);

	if (OPT_GET("Video/Detached/Enabled")->GetBool() && !ui.dialog->Get<DialogDetachedVideo>())
		cmd::call("video/detach", context.get());
	Thaw();
}

void FrameMain::SetDisplayMode(int video, int audio) {
	if (!IsShownOnScreen()) return;

	bool sv = false, sa = false;
	auto core = context->GetCore();
	auto ui = context->GetUI();

	if (video == -1) sv = showVideo;
	else if (video)  sv = core.project->VideoProvider() && !ui.dialog->Get<DialogDetachedVideo>();

	if (audio == -1) sa = showAudio;
	else if (audio)  sa = !!core.project->AudioProvider();

	// See if anything changed
	if (sv == showVideo && sa == showAudio) return;

	bool didFreeze = !IsFrozen();
	if (didFreeze) Freeze();

	if (sv)
		EnsureVideoBoxCreated();
	if (sa)
		EnsureAudioBoxCreated();

	showVideo = sv;
	showAudio = sa;

	core.videoController->Stop();

	if (videoBox)
		TopSizer->Show(videoBox, showVideo, true);
	if (audioBox)
		ToolsSizer->Show(audioBox, showAudio, true);

	MainSizer->Layout();
	Layout();

	if (didFreeze) Thaw();
}

void FrameMain::UpdateTitle() {
	wxString newTitle;
	auto core = context->GetCore();
	if (core.subsController->IsModified()) newTitle << wxS("* ");
	newTitle << core.subsController->Filename().filename().wstring();

#ifndef __WXMAC__
	newTitle << wxS(" - Aegisub ") << wxString::FromUTF8(GetAegisubLongVersionString());
#endif

#if defined(__WXMAC__)
	// On Mac, set the mark in the close button
	OSXSetModified(core.subsController->IsModified());
#endif

	if (GetTitle() != newTitle) SetTitle(newTitle);
}

void FrameMain::OnVideoOpen(AsyncVideoProvider *provider) {
	if (!provider) {
		SetDisplayMode(0, -1);
		return;
	}

	if (!videoBox) {
		if (!pending_video_open_ui_sync) {
			pending_video_open_ui_sync = true;
			CallAfter([this] { SyncVideoOpenUi(); });
		}
		return;
	}

	SyncVideoOpenUi();
}

void FrameMain::OnVideoDetach(agi::OptionValue const& opt) {
	auto core = context->GetCore();
	if (opt.GetBool())
		SetDisplayMode(0, -1);
	else if (core.project->VideoProvider())
		SetDisplayMode(1, -1);
}

void FrameMain::StatusTimeout(wxString text,int ms) {
	SetStatusText(text,1);
	StatusClear.SetOwner(this, ID_APP_TIMER_STATUSCLEAR);
	StatusClear.Start(ms,true);
}

BEGIN_EVENT_TABLE(FrameMain, wxFrame)
	EVT_TIMER(ID_APP_TIMER_STATUSCLEAR, FrameMain::OnStatusClear)
#ifdef _WIN32
	EVT_TIMER(ID_APP_TIMER_FONTCHANGE_DEBOUNCE, FrameMain::OnFontChangeDebounce)
	EVT_TIMER(ID_APP_TIMER_AUDIO_OUTPUT_RECOVERY, FrameMain::OnAudioOutputRecoveryDebounce)
#endif
	EVT_CLOSE(FrameMain::OnCloseWindow)
	EVT_CHAR_HOOK(FrameMain::OnKeyDown)
	EVT_MOUSEWHEEL(FrameMain::OnMouseWheel)
END_EVENT_TABLE()

void FrameMain::OnCloseWindow(wxCloseEvent &event) {
	wxEventBlocker blocker(this, wxEVT_CLOSE_WINDOW);
	auto core = context->GetCore();
	auto ui = context->GetUI();

	core.videoController->Stop();
	core.audioController->Stop();

	// Ask user if he wants to save first
	if (core.subsController->TryToClose(event.CanVeto()) == wxCANCEL) {
		event.Veto();
		return;
	}

	ui.dialog.reset();

	// Store maximization state
	OPT_SET("App/Maximized")->SetBool(IsMaximized());

#ifdef _WIN32
	FontChangeDebounce.Stop();
	AudioOutputRecoveryDebounce.Stop();
	UnregisterSessionNotifications();
#endif

	Destroy();
}

void FrameMain::OnStatusClear(wxTimerEvent &) {
	SetStatusText(wxString(),1);
}

#ifdef _WIN32
void FrameMain::OnFontChangeDebounce(wxTimerEvent &) {
	context->GetCore().project->ReloadSubtitlesProvider();
}

void FrameMain::OnAudioOutputRecoveryDebounce(wxTimerEvent &) {
	LOG_I("audio/player/xaudio2/recovery") << "Running queued XAudio2 recovery after " << pending_audio_output_recovery_reason;
	context->GetCore().audioController->RecoverAudioPlayerAfterDeviceChange();
	pending_audio_output_recovery_reason.clear();
}

void FrameMain::QueueAudioOutputRecovery(std::string reason) {
	if (OPT_GET("Audio/Player")->GetString() != "XAudio2")
		return;

	if (AudioOutputRecoveryDebounce.IsRunning()) {
		LOG_D("audio/player/xaudio2/recovery") << "Coalescing XAudio2 recovery request; latest trigger: " << reason;
	}
	else {
		LOG_I("audio/player/xaudio2/recovery") << "Queueing XAudio2 recovery after " << reason;
	}
	pending_audio_output_recovery_reason = std::move(reason);
	AudioOutputRecoveryDebounce.SetOwner(this, ID_APP_TIMER_AUDIO_OUTPUT_RECOVERY);
	AudioOutputRecoveryDebounce.Start(kAudioOutputRecoveryDebounceDelayMs, true);
}

void FrameMain::RegisterSessionNotifications() {
	if (session_notifications_registered)
		return;

	auto *hwnd = reinterpret_cast<HWND>(GetHandle());
	if (!hwnd)
		return;

	session_notifications_registered = !!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
	if (!session_notifications_registered)
		LOG_W("audio/player/xaudio2/recovery") << "Failed to register session-change notifications for XAudio2 recovery.";
}

void FrameMain::UnregisterSessionNotifications() {
	if (!session_notifications_registered)
		return;

	auto *hwnd = reinterpret_cast<HWND>(GetHandle());
	if (hwnd)
		WTSUnRegisterSessionNotification(hwnd);
	session_notifications_registered = false;
}

WXLRESULT FrameMain::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
	if (message == WM_FONTCHANGE) {
		FontChangeDebounce.SetOwner(this, ID_APP_TIMER_FONTCHANGE_DEBOUNCE);
		FontChangeDebounce.Start(kFontChangeDebounceDelayMs, true);
	}

	if (message == WM_DEVICECHANGE && IsRelevantAudioDeviceChange(wParam))
		QueueAudioOutputRecovery(DescribeAudioDeviceChange(wParam));

	if (message == WM_WTSSESSION_CHANGE && IsRelevantSessionChange(wParam))
		QueueAudioOutputRecovery(DescribeSessionChange(wParam));

	if (message == WM_SIZE) {
		WXLRESULT res = wxFrame::MSWWindowProc(message, wParam, lParam);
		// Invalidate the entire frame after resize to prevent black areas.
		// With WS_CLIPCHILDREN, newly-exposed gaps between the panel's old
		// position and the frame's new edge are not repainted by default.
		Refresh(false);
		return res;
	}

	return wxFrame::MSWWindowProc(message, wParam, lParam);
}
#endif

void FrameMain::OnAudioOpen(agi::AudioProvider *provider) {
	if (!provider) {
		SetDisplayMode(-1, 0);
		return;
	}

	if (!audioBox) {
		if (!pending_audio_open_ui_sync) {
			pending_audio_open_ui_sync = true;
			CallAfter([this] { SyncAudioOpenUi(); });
		}
		return;
	}

	SetDisplayMode(-1, 1);
}

void FrameMain::OnSubtitlesOpen() {
	UpdateTitle();
	SetDisplayMode(1, 1);
}

void FrameMain::OnKeyDown(wxKeyEvent &event) {
	hotkey::check("Main Frame", context.get(), event);
}

void FrameMain::OnMouseWheel(wxMouseEvent &evt) {
	ForwardMouseWheelEvent(this, evt);
}
