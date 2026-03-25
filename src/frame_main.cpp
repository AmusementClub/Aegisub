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
#include "dialog_progress.h"
#include "dialogs.h"
#include "libresrc/libresrc.h"
#include "main.h"
#include "options.h"
#include "project.h"
#include "status_sink.h"
#include "subs_controller.h"
#include "subs_edit_box.h"
#include "ui_services.h"
#include "utils.h"
#include "version.h"
#include "video_box.h"
#include "video_controller.h"
#include "video_display.h"
#include "wx_ui_services.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <wx/dnd.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/sysopt.h>

enum {
	ID_APP_TIMER_STATUSCLEAR = 12002
};

#ifdef WITH_STARTUPLOG
#define StartupLog(a) wxMessageBox(wxS(a), wxS("Aegisub startup log"))
#else
#define StartupLog(a) LOG_I("frame_main/init") << a
#endif

namespace {
int to_wx_flags(agi::InteractionButtons buttons, agi::InteractionIcon icon) {
	int flags = 0;
	switch (buttons) {
	case agi::InteractionButtons::Ok:
		flags |= wxOK;
		break;
	case agi::InteractionButtons::OkCancel:
		flags |= wxOK | wxCANCEL;
		break;
	case agi::InteractionButtons::YesNo:
		flags |= wxYES_NO;
		break;
	case agi::InteractionButtons::YesNoCancel:
		flags |= wxYES_NO | wxCANCEL;
		break;
	}

	switch (icon) {
	case agi::InteractionIcon::None:
		break;
	case agi::InteractionIcon::Info:
		flags |= wxICON_INFORMATION;
		break;
	case agi::InteractionIcon::Warning:
		flags |= wxICON_WARNING;
		break;
	case agi::InteractionIcon::Error:
		flags |= wxICON_ERROR;
		break;
	case agi::InteractionIcon::Question:
		flags |= wxICON_QUESTION;
		break;
	}

	return flags | wxCENTER;
}

agi::InteractionResult from_wx_result(int result) {
	switch (result) {
	case wxOK:
		return agi::InteractionResult::Ok;
	case wxCANCEL:
		return agi::InteractionResult::Cancel;
	case wxYES:
		return agi::InteractionResult::Yes;
	case wxNO:
		return agi::InteractionResult::No;
	default:
		return agi::InteractionResult::Cancel;
	}
}

agi::InteractionResult safe_result(agi::InteractionButtons buttons) {
	switch (buttons) {
	case agi::InteractionButtons::Ok:
		return agi::InteractionResult::Ok;
	case agi::InteractionButtons::OkCancel:
		return agi::InteractionResult::Cancel;
	case agi::InteractionButtons::YesNo:
		return agi::InteractionResult::No;
	case agi::InteractionButtons::YesNoCancel:
		return agi::InteractionResult::Cancel;
	}
	return agi::InteractionResult::Cancel;
}

class FrameMainStatusSink final : public agi::StatusSink {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;

public:
	FrameMainStatusSink(FrameMain *frame, agi::ui::WeakLifetime lifetime)
	: frame(frame)
	, lifetime(std::move(lifetime))
	{
	}

	void ShowStatus(std::string const& message, int timeout_ms) override {
		agi::ui::MainAsyncIfAlive(lifetime, [frame = frame, message, timeout_ms] {
			frame->StatusTimeout(to_wx(message), timeout_ms);
		});
	}
};

class FrameMainNotificationSink final : public agi::NotificationSink {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;

public:
	FrameMainNotificationSink(FrameMain *frame, agi::ui::WeakLifetime lifetime)
	: frame(frame)
	, lifetime(std::move(lifetime))
	{
	}

	void ShowInfo(std::string const& title, std::string const& message) override {
		agi::ui::MainInvokeIfAlive(lifetime, [frame = frame, title, message] {
			wxMessageBox(to_wx(message), to_wx(title), wxOK | wxICON_INFORMATION | wxCENTER, frame);
		});
	}

	void ShowError(std::string const& title, std::string const& message) override {
		agi::ui::MainInvokeIfAlive(lifetime, [frame = frame, title, message] {
			wxMessageBox(to_wx(message), to_wx(title), wxOK | wxICON_ERROR | wxCENTER, frame);
		});
	}

	void ShowWarning(std::string const& title, std::string const& message) override {
		agi::ui::MainInvokeIfAlive(lifetime, [frame = frame, title, message] {
			wxMessageBox(to_wx(message), to_wx(title), wxOK | wxICON_WARNING | wxCENTER, frame);
		});
	}
};

class FrameMainInteractionSink final : public agi::InteractionSink {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;

public:
	FrameMainInteractionSink(FrameMain *frame, agi::ui::WeakLifetime lifetime)
	: frame(frame)
	, lifetime(std::move(lifetime))
	{
	}

	agi::InteractionResult Request(agi::InteractionRequest const& request) override {
		return agi::ui::MainInvoke([frame = frame, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return safe_result(request.buttons);

			return from_wx_result(wxMessageBox(
				to_wx(request.message),
				to_wx(request.title),
				to_wx_flags(request.buttons, request.icon),
				frame));
		});
	}
};

class FrameMainSingleChoiceInteractionSink final : public agi::SingleChoiceInteractionSink {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;

public:
	FrameMainSingleChoiceInteractionSink(FrameMain *frame, agi::ui::WeakLifetime lifetime)
	: frame(frame)
	, lifetime(std::move(lifetime))
	{
	}

	std::optional<int> RequestSingleChoice(agi::SingleChoiceInteractionRequest const& request) override {
		return agi::ui::MainInvoke([frame = frame, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return std::optional<int>();
			return agi::ShowSingleChoiceDialog(frame, request);
		});
	}
};

class FrameMainVideoSourceRequestService final : public agi::VideoSourceRequestService {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;

public:
	FrameMainVideoSourceRequestService(FrameMain *frame, agi::ui::WeakLifetime lifetime)
	: frame(frame)
	, lifetime(std::move(lifetime))
	{
	}

	agi::fs::path RequestOpenVideoFile(agi::OpenFileDialogRequest const& request) override {
		return agi::ui::MainInvoke([frame = frame, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return agi::fs::path();
			return OpenFileSelector(
				to_wx(request.title),
				request.option_name,
				request.default_filename,
				request.default_extension,
				request.wildcard,
				frame);
		});
	}

	std::string RequestDummyVideoPath() override {
		return agi::ui::MainInvoke([frame = frame, lifetime = lifetime] {
			if (!lifetime.lock())
				return std::string();
			return CreateDummyVideo(frame);
		});
	}
};

class FrameMainBackgroundRunner final : public agi::BackgroundRunner {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;
	std::string title;
	std::string message;

public:
	FrameMainBackgroundRunner(FrameMain *frame, agi::ui::WeakLifetime lifetime, std::string title, std::string message)
	: frame(frame)
	, lifetime(std::move(lifetime))
	, title(std::move(title))
	, message(std::move(message))
	{
	}

	void Run(std::function<void(agi::ProgressSink *)> task) override {
		agi::ui::MainInvoke([this, task = std::move(task)]() mutable {
			if (!lifetime.lock()) {
				agi::detail::InlineBackgroundRunner fallback;
				fallback.Run(std::move(task));
				return;
			}

			DialogProgress dialog(frame, to_wx(title), to_wx(message));
			dialog.Run(std::move(task));
		});
	}
};

class FrameMainBackgroundRunnerFactory final : public agi::BackgroundRunnerFactory {
	FrameMain *frame = nullptr;
	agi::ui::WeakLifetime lifetime;

public:
	FrameMainBackgroundRunnerFactory(FrameMain *frame, agi::ui::WeakLifetime lifetime)
	: frame(frame)
	, lifetime(std::move(lifetime))
	{
	}

	std::unique_ptr<agi::BackgroundRunner> Create(std::string const& title, std::string const& message) override {
		return agi::make_unique<FrameMainBackgroundRunner>(frame, lifetime, title, message);
	}
};
}

/// Handle files drag and dropped onto Aegisub
class AegisubFileDropTarget final : public wxFileDropTarget {
	agi::Context *context;
	agi::ui::WeakLifetime lifetime;
public:
	AegisubFileDropTarget(agi::Context *context, agi::ui::WeakLifetime lifetime)
	: context(context)
	, lifetime(std::move(lifetime)) {
	}
	bool OnDropFiles(wxCoord, wxCoord, wxArrayString const& filenames) override {
		std::vector<agi::fs::path> files;
		for (wxString const& fn : filenames)
			files.push_back(from_wx(fn));
		agi::ui::MainAsyncIfAlive(lifetime, [context = context, files = std::move(files)] {
			context->GetCore().project->LoadList(files);
		});
		return true;
	}
};

FrameMain::FrameMain()
: wxFrame(nullptr, -1, wxEmptyString, wxDefaultPosition, wxSize(920,700), wxDEFAULT_FRAME_STYLE | wxCLIP_CHILDREN)
, context(agi::make_unique<agi::Context>())
{
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

	StartupLog("Initializing context frames");
	ui.parent = this;
	ui.frame = this;
	core.statusSink = std::make_shared<FrameMainStatusSink>(this, GetAsyncUiLifetime());
	core.notificationSink = std::make_shared<FrameMainNotificationSink>(this, GetAsyncUiLifetime());
	core.interactionSink = std::make_shared<FrameMainInteractionSink>(this, GetAsyncUiLifetime());
	core.singleChoiceInteractionSink = std::make_shared<FrameMainSingleChoiceInteractionSink>(this, GetAsyncUiLifetime());
	core.videoSourceRequestService = std::make_shared<FrameMainVideoSourceRequestService>(this, GetAsyncUiLifetime());
	core.backgroundRunnerFactory = std::make_shared<FrameMainBackgroundRunnerFactory>(this, GetAsyncUiLifetime());

	StartupLog("Apply saved Maximized state");
	if (OPT_GET("App/Maximized")->GetBool()) Maximize(true);

	StartupLog("Initialize toolbar");
	wxSystemOptions::SetOption(wxS("msw.remap"), 0);
	OPT_SUB("App/Show Toolbar", &FrameMain::EnableToolBar, this);
	EnableToolBar(*OPT_GET("App/Show Toolbar"));

	StartupLog("Initialize menu bar");
	menu::GetMenuBar("main", this, (wxID_HIGHEST + 1) + 10000, context.get());

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

	StartupLog("Set up drag/drop target");
	SetDropTarget(new AegisubFileDropTarget(context.get(), GetAsyncUiLifetime()));

	StartupLog("Load default file");
	core.project->CloseSubtitles();

	StartupLog("Display main window");
	AddFullScreenButton(this);
	Show();
	SetDisplayMode(1, 1);

	StartupLog("Leaving FrameMain constructor");
}

FrameMain::~FrameMain () {
	ui_activation.Deactivate();
	auto core = context->GetCore();
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
	StartupLog("Create background panel");
	auto Panel = new wxPanel(this, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);

	StartupLog("Create subtitles grid");
	auto ui = context->GetUI();
	ui.subsGrid = new BaseGrid(Panel, context.get());

	StartupLog("Create video box");
	videoBox = new VideoBox(Panel, false, context.get());

	StartupLog("Create audio box");
	ui.audioBox = audioBox = new AudioBox(Panel, context.get());

	StartupLog("Create subtitle editing box");
	auto EditBox = new SubsEditBox(Panel, context.get());

	StartupLog("Arrange main sizers");
	ToolsSizer = new wxBoxSizer(wxVERTICAL);
	ToolsSizer->Add(audioBox, 0, wxEXPAND);
	ToolsSizer->Add(EditBox, 1, wxEXPAND);
	TopSizer = new wxBoxSizer(wxHORIZONTAL);
	TopSizer->Add(videoBox, 0, wxEXPAND, 0);
	TopSizer->Add(ToolsSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
	MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(new wxStaticLine(Panel),0,wxEXPAND | wxALL,0);
	MainSizer->Add(TopSizer,0,wxEXPAND | wxALL,0);
	MainSizer->Add(ui.subsGrid,1,wxEXPAND | wxALL,0);
	Panel->SetSizer(MainSizer);

	StartupLog("Perform layout");
	Layout();
	StartupLog("Leaving InitContents");
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

	showVideo = sv;
	showAudio = sa;

	bool didFreeze = !IsFrozen();
	if (didFreeze) Freeze();

	core.videoController->Stop();

	TopSizer->Show(videoBox, showVideo, true);
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

	Freeze();
	int vidx = provider->GetWidth(), vidy = provider->GetHeight();
	auto ui = context->GetUI();

	// Set zoom level based on video resolution and window size
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

	Destroy();
}

void FrameMain::OnStatusClear(wxTimerEvent &) {
	SetStatusText(wxString(),1);
}

void FrameMain::OnAudioOpen(agi::AudioProvider *provider) {
	if (provider)
		SetDisplayMode(-1, 1);
	else
		SetDisplayMode(-1, 0);
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
