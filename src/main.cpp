// Copyright (c) 2005, Rodrigo Braz Monteiro
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

/// @file main.cpp
/// @brief Main entry point, as well as crash handling
/// @ingroup main
///

#include "main.h"

#include "command/command.h"
#include "include/aegisub/hotkey.h"

#include "auto4_base.h"
#include "app_runtime.h"
#include "compat.h"
#include "crash_writer.h"
#include "format.h"
#include "frame_main.h"
#include "gui_wx_runtime_entry_host.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "project.h"
#include "subs_controller.h"
#include "utils.h"
#include <libaegisub/dispatch.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <vector>
#include <wx/arrstr.h>
#include <wx/clipbrd.h>
#include <wx/msgdlg.h>
#include <wx/stackwalk.h>
#include <wx/thread.h>
#include <wx/utils.h>

wxIMPLEMENT_APP_NO_MAIN(AegisubApp);

static const char *LastStartupState = nullptr;

#ifdef WITH_STARTUPLOG
#define StartupLog(a) wxMessageBox(wxT(a), wxT("Aegisub startup log"))
#else
#define StartupLog(a) LastStartupState = a
#endif

void AegisubApp::OnAssertFailure(const wxChar *file, int line, const wxChar *func, const wxChar *cond, const wxChar *msg) {
	auto narrow = [](const wxChar *value) {
		return value ? from_wx(wxString(value)) : std::string();
	};
	LOG_A("wx/assert") << narrow(file) << ":" << line << ":" << narrow(func) << "() " << narrow(cond) << ": " << narrow(msg);
	wxApp::OnAssertFailure(file, line, func, cond, msg);
}

AegisubApp::AegisubApp() {
	// http://trac.wxwidgets.org/ticket/14302
	wxSetEnv(wxS("UBUNTU_MENUPROXY"), wxS("0"));
}

namespace {

std::vector<std::string> ToUtf8Args(wxArrayString const& args) {
	std::vector<std::string> values;
	values.reserve(args.size());
	for (auto const& arg : args)
		values.emplace_back(arg.ToStdString(wxConvUTF8));
	return values;
}

}

/// Message displayed when an exception has occurred.
static wxString exception_message = wxS("Oops, Aegisub has crashed!\n\nAn attempt has been made to save a copy of your file to:\n\n%s\n\nAegisub will now close.");

/// @brief Gets called when application starts.
/// @return bool
bool AegisubApp::OnInit() {
	// App name (yeah, this is a little weird to get rid of an odd warning)
#if defined(__WXMSW__) || defined(__WXMAC__)
	SetAppName(wxS("Aegisub"));
#else
	SetAppName(wxS("aegisub"));
#endif

	BindGuiWxMainQueueDispatchHandler([this] { OnExceptionInMainLoop(); });

	runtime = std::make_unique<AppRuntime>();
	std::string runtime_error;
	auto runtime_options = BuildGuiWxAppRuntimeInitOptions();
	auto bootstrap_ui_host = runtime_options.bootstrap_ui_host;
	runtime_options.bootstrap_ui_host = bootstrap_ui_host;
	if (!runtime->Initialize(std::move(runtime_options), runtime_error)) {
		ShowGuiWxBootstrapUiError("Fatal error while initializing", runtime_error);
		return false;
	}

	StartupLog("Inside OnInit");
	try {
		// Crash handling
#if (!defined(_DEBUG) || defined(WITH_EXCEPTIONS)) && (wxUSE_ON_FATAL_EXCEPTION+0)
		StartupLog("Install exception handler");
		wxHandleFatalExceptions(true);
#endif

#ifdef __APPLE__
		// When run from an app bundle, LC_CTYPE defaults to "C", which breaks on
		// anything involving unicode and in some cases number formatting.
		// The right thing to do here would be to query CoreFoundation for the user's
		// locale and add .UTF-8 to that, but :effort:
		setlocale(LC_CTYPE, "en_US.UTF-8");
#endif

		exception_message = _("Oops, Aegisub has crashed!\n\nAn attempt has been made to save a copy of your file to:\n\n%s\n\nAegisub will now close.");

		StartupLog("Create main window");
		StartupLog("Possibly perform automatic updates check");
		StartupLog("Parse command line");
		RunGuiWxAppStartupSequence(ToUtf8Args(argv.GetArguments()),
			[this] { NewProjectContext(); },
			[this](std::vector<std::string> const& files) {
				std::vector<agi::fs::path> paths;
				paths.reserve(files.size());
				for (auto const& file : files)
					paths.emplace_back(file);
				if (!paths.empty())
					frames[0]->context->GetCore().project->LoadList(paths);
			});
	}
	catch (agi::Exception const& e) {
		ShowGuiWxBootstrapUiError("Fatal error while initializing", e.GetMessage());
		return false;
	}
	catch (std::exception const& e) {
		ShowGuiWxBootstrapUiError("Fatal error while initializing", e.what());
		return false;
	}
#ifndef _DEBUG
	catch (...) {
		ShowGuiWxBootstrapUiError("Fatal error while initializing", "Unhandled exception");
		return false;
	}
#endif

	StartupLog("Clean old autosave files");
	CleanCache(config::path->Decode(OPT_GET("Path/Auto/Save")->GetString()), "*.AUTOSAVE.ass", 100, 1000);

	StartupLog("Initialization complete");
	return true;
}

int AegisubApp::OnExit() {
	ui_activation.Deactivate();

	for (auto frame : frames)
		delete frame;
	frames.clear();

	if (wxTheClipboard->Open()) {
		wxTheClipboard->Flush();
		wxTheClipboard->Close();
	}

	runtime.reset();

	return wxApp::OnExit();
}

agi::Context& AegisubApp::NewProjectContext() {
	auto frame = new FrameMain;
	frame->Bind(wxEVT_DESTROY, [=](wxWindowDestroyEvent& evt) {
		if (evt.GetWindow() != frame) {
			evt.Skip();
			return;
		}

		frames.erase(remove(begin(frames), end(frames), frame), end(frames));
		if (frames.empty()) {
			ExitMainLoop();
		}
	});
	frames.push_back(frame);
	return *frame->context;
}

void AegisubApp::CloseAll() {
	for (auto frame : frames) {
		if (!frame->Close())
			break;
	}
}

void AegisubApp::UnhandledException(bool stackWalk) {
#if (!defined(_DEBUG) || defined(WITH_EXCEPTIONS)) && (wxUSE_ON_FATAL_EXCEPTION+0)
	bool any = false;
	agi::fs::path path;
	for (auto& frame : frames) {
		auto c = frame->context.get();
		if (!c) continue;

		auto core = c->GetCore();
		if (!core.ass || !core.subsController) continue;

		path = config::path->Decode("?user/recovered");
		agi::fs::CreateDirectory(path);

		auto filename = core.subsController->Filename().stem();
		filename.replace_extension(agi::format("%s.ass", agi::util::strftime("%Y-%m-%d-%H-%M-%S")));
		path /= filename;
		core.subsController->Save(path);

		any = true;
	}

	if (stackWalk)
		crash_writer::Write();

	if (any) {
		// Inform user of crash.
		ShowGuiWxBootstrapUiError(from_wx(_("Program error")), agi::format(exception_message, path));
	}
	else if (LastStartupState) {
		ShowGuiWxBootstrapUiError(
			from_wx(_("Program error")),
			agi::format("Aegisub has crashed while starting up!\n\nThe last startup step attempted was: %s.", LastStartupState));
	}
#endif
}

void AegisubApp::OnUnhandledException() {
	UnhandledException(false);
}

void AegisubApp::OnFatalException() {
	UnhandledException(true);
}

bool AegisubApp::OnExceptionInMainLoop() {
	try {
		throw;
	}
	catch (const agi::Exception &e) {
		ShowGuiWxBootstrapUiError(
			"Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), e.GetMessage()));
	}
	catch (const std::exception &e) {
		ShowGuiWxBootstrapUiError(
			"Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), e.what()));
	}
	catch (...) {
		ShowGuiWxBootstrapUiError(
			"Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), "Unknown error"));
	}
	return true;
}

int AegisubApp::OnRun() {
	std::string error;

	try {
		return MainLoop();
	}
	catch (const std::exception &e) { error = std::string("std::exception: ") + e.what(); }
	catch (const agi::Exception &e) { error = "agi::exception: " + e.GetMessage(); }
	catch (...) { error = "Program terminated in error."; }

	// Report errors
	if (!error.empty()) {
		crash_writer::Write(error);
		OnUnhandledException();
	}

	ExitMainLoop();
	return 1;
}

void AegisubApp::MacOpenFiles(wxArrayString const& filenames) {
	OpenFiles(filenames);
}

void AegisubApp::OpenFiles(wxArrayStringsAdapter filenames) {
	std::vector<agi::fs::path> files;
	for (size_t i = 0; i < filenames.GetCount(); ++i)
		files.push_back(from_wx(filenames[i]));
	if (!files.empty())
		frames[0]->context->GetCore().project->LoadList(files);
}
