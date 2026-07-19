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
#include "automation_command_executor.h"
#include "automation_scenario.h"
#include "automation_scenario_runner.h"
#include "avisynth_provider_registration.h"
#include "compat.h"
#include "crash_writer.h"
#include "format.h"
#include "frame_main.h"
#include "gui_wx_runtime_entry_host.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "subs_controller.h"
#include "utils.h"
#include <libaegisub/dispatch.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
	auto const started = std::chrono::steady_clock::now();

	// http://trac.wxwidgets.org/ticket/14302
	wxSetEnv(wxS("UBUNTU_MENUPROXY"), wxS("0"));

	perf_trace::ObserveWindowOpenPhase(
		"main",
		"startup.wx_app.constructor",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

namespace {

agi::fs::path PathFromUtf8(std::string const& value) {
	return agi::fs::PathFromString(value);
}

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
	auto const startup_started = std::chrono::steady_clock::now();
	auto duration_ms = [](std::chrono::steady_clock::time_point started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	};
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase("main", phase, duration_ms(phase_started));
		phase_started = std::chrono::steady_clock::now();
	};
	auto finish_startup_trace = [&](bool succeeded) {
		perf_trace::TraceWindowOpenEnd("main", duration_ms(startup_started), succeeded);
	};
	perf_trace::TraceWindowOpenBegin("main");

	// App name (yeah, this is a little weird to get rid of an odd warning)
#if defined(__WXMSW__) || defined(__WXMAC__)
	SetAppName(wxS("Aegisub"));
#else
	SetAppName(wxS("aegisub"));
#endif
	observe_phase("startup.on_init.set_app_name");
	auto const process_args = ToUtf8Args(argv.GetArguments());
	launch_plan.emplace(ParseAppLaunchPlan(process_args));
	if (launch_plan->mode == AppLaunchMode::GuiTest) {
		if (!launch_plan->ParseSucceeded() || !launch_plan->gui_test_run) {
			finish_startup_trace(false);
			ShowGuiWxBootstrapUiError("Invalid GUI test command line", launch_plan->error);
			return false;
		}
		std::string profile_error;
		auto const& request = *launch_plan->gui_test_run;
		automation_profile = std::make_unique<AutomationRuntimeProfile>(
			AutomationRuntimeProfile::Create(
				AutomationRuntimeProfileOptions{
					request.profile_directory,
					request.artifacts_directory,
					request.keep_profile},
				profile_error));
		if (automation_profile->Root().empty()) {
			finish_startup_trace(false);
			ShowGuiWxBootstrapUiError("Could not create GUI test profile", profile_error);
			return false;
		}
	}
	auto record_gui_test_phase = [&](char const* phase) {
		if (!automation_profile)
			return;
		try {
			std::ofstream out(automation_profile->ArtifactsDirectory() / agi::fs::PathFromString("startup.log"), std::ios::app);
			out << phase << "\n";
		}
		catch (...) {
		}
	};
	record_gui_test_phase("launch-plan.ready");

	BindGuiWxMainQueueDispatchHandler([this] { OnExceptionInMainLoop(); });
	observe_phase("startup.on_init.bind_main_queue_handler");

	runtime = std::make_unique<AppRuntime>();
	observe_phase("startup.on_init.create_runtime");
	std::string runtime_error;
	auto runtime_options = BuildGuiWxAppRuntimeInitOptions();
	if (automation_profile) {
		runtime_options.path_overrides = automation_profile->PathOverrides();
		runtime_options.locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
		runtime_options.load_global_scripts = true;
	}
	record_gui_test_phase("runtime-options.ready");
	auto bootstrap_ui_host = runtime_options.bootstrap_ui_host;
	runtime_options.bootstrap_ui_host = bootstrap_ui_host;
	observe_phase("startup.on_init.build_runtime_options");
	auto const runtime_initialize_started = std::chrono::steady_clock::now();
	if (!runtime->Initialize(std::move(runtime_options), runtime_error)) {
		perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.total", duration_ms(runtime_initialize_started));
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", runtime_error);
		return false;
	}
	RegisterAvisynthProviderFactories();
	if (automation_profile) {
		OPT_SET("App/First Start")->SetBool(false);
		OPT_SET("App/Auto/Check For Updates")->SetBool(false);
		OPT_SET("App/Auto/Save")->SetBool(false);
	}
	record_gui_test_phase("runtime.initialized");
	perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.total", duration_ms(runtime_initialize_started));
	phase_started = std::chrono::steady_clock::now();

	StartupLog("Inside OnInit");
	try {
		// Crash handling
#if (!defined(_DEBUG) || defined(WITH_EXCEPTIONS)) && (wxUSE_ON_FATAL_EXCEPTION+0)
		StartupLog("Install exception handler");
		wxHandleFatalExceptions(true);
		observe_phase("startup.on_init.install_exception_handler");
#endif

#ifdef __APPLE__
		// When run from an app bundle, LC_CTYPE defaults to "C", which breaks on
		// anything involving unicode and in some cases number formatting.
		// The right thing to do here would be to query CoreFoundation for the user's
		// locale and add .UTF-8 to that, but :effort:
		setlocale(LC_CTYPE, "en_US.UTF-8");
		observe_phase("startup.on_init.platform_locale_adjustment");
#endif

		exception_message = _("Oops, Aegisub has crashed!\n\nAn attempt has been made to save a copy of your file to:\n\n%s\n\nAegisub will now close.");
		observe_phase("startup.on_init.exception_message_setup");

		StartupLog("Create main window");
		StartupLog("Possibly perform automatic updates check");
		StartupLog("Parse command line");
		auto const startup_sequence_started = std::chrono::steady_clock::now();
		auto startup_args = process_args;
		if (launch_plan->mode == AppLaunchMode::GuiTest) {
			startup_args.resize(1);
			startup_args.insert(
				startup_args.end(),
				launch_plan->gui_test_open_files.begin(),
				launch_plan->gui_test_open_files.end());
		}
		RunGuiWxAppStartupSequence(startup_args,
			[this] { NewProjectContext(); },
			[this](std::vector<std::string> const& files) {
				std::vector<agi::fs::path> paths;
				paths.reserve(files.size());
				for (auto const& file : files)
					paths.push_back(PathFromUtf8(file));
				if (!paths.empty())
					frames[0]->context->GetCore().project->LoadList(paths);
			},
			launch_plan->mode != AppLaunchMode::GuiTest);
		record_gui_test_phase("frame.created");
		perf_trace::ObserveWindowOpenPhase("main", "startup.sequence.total", duration_ms(startup_sequence_started));
		if (launch_plan->mode == AppLaunchMode::GuiTest)
			CallAfter([this] { StartGuiTest(); });
	}
	catch (agi::Exception const& e) {
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", e.GetMessage());
		return false;
	}
	catch (std::exception const& e) {
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", e.what());
		return false;
	}
#ifndef _DEBUG
	catch (...) {
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", "Unhandled exception");
		return false;
	}
#endif

	StartupLog("Clean old autosave files");
	auto const autosave_cleanup_started = std::chrono::steady_clock::now();
	CleanCache(config::path->Decode(OPT_GET("Path/Auto/Save")->GetString()), "*.AUTOSAVE.ass", 100, 1000);
	perf_trace::ObserveWindowOpenPhase("main", "startup.post.autosave_cleanup.schedule", duration_ms(autosave_cleanup_started));

	StartupLog("Initialization complete");
	finish_startup_trace(true);
	return true;
}

void AegisubApp::StartGuiTest() {
	if (!automation_profile || !launch_plan || frames.empty())
		return;

	auto const artifacts = automation_profile->ArtifactsDirectory();
	auto save_json = [&](std::string const& name, json::Object object) {
		agi::fs::path temporary_path;
		try {
			auto final_path = artifacts / agi::fs::PathFromString(name);
			temporary_path = agi::fs::UniquePath(
				artifacts / agi::fs::PathFromString(name + ".tmp-%%%%%%%%"));
			{
				auto stream = agi::io::Save(temporary_path);
				agi::JsonWriter::Write(object, stream.Get());
				stream.Get().flush();
			}
			std::error_code error;
			std::filesystem::remove(final_path, error);
			std::filesystem::rename(temporary_path, final_path, error);
			if (error)
				throw std::system_error(error, "could not publish automation artifact");
			return true;
		}
		catch (std::exception const& e) {
			if (!temporary_path.empty()) {
				std::error_code ignored;
				std::filesystem::remove(temporary_path, ignored);
			}
			LOG_E("automation/gui_test") << "Could not save " << name << ": " << e.what();
			return false;
		}
	};

	json::Object ready;
	ready["version"] = static_cast<int64_t>(1);
	ready["host"] = "gui-test";
	ready["state"] = "ready";
	ready["process_id"] = static_cast<int64_t>(wxGetProcessId());
	ready["window_title"] = from_wx(frames.front()->GetTitle());
	ready["artifacts"] = agi::fs::PathToGenericString(artifacts);
	if (!save_json("ready.json", std::move(ready))) {
		gui_test_exit_code = 2;
		ScheduleGuiTestClose();
		return;
	}

	if (launch_plan->gui_test_host)
		return;

	auto const& request = *launch_plan->gui_test_run;
	auto scenario_result = aegisub::automation_scenario::Load(request.scenario_path, request.inputs);
	if (!scenario_result.scenario) {
		json::Object failure;
		failure["version"] = static_cast<int64_t>(1);
		failure["host"] = "gui-test";
		failure["passed"] = false;
		failure["exit_code"] = static_cast<int64_t>(64);
		failure["error"] = scenario_result.error;
		save_json("result.json", std::move(failure));
		gui_test_exit_code = 64;
		ScheduleGuiTestClose();
		return;
	}

	auto execution = aegisub::automation_scenario_runner::Run(
		*scenario_result.scenario,
		"gui-test",
		artifacts,
		{},
		[context = frames.front()->context.get()](std::string const& command_id) {
			return aegisub::automation_command_executor::Invoke(command_id, *context, true);
		});
	auto const passed = execution.passed;
	auto const exit_code = execution.exit_code;
	auto result = aegisub::automation_scenario_runner::SerializeResult(
		scenario_result.scenario->name,
		"gui-test",
		std::move(execution),
		automation_profile->Root(),
		artifacts);
	if (!save_json("result.json", std::move(result))) {
		gui_test_exit_code = 2;
		ScheduleGuiTestClose();
		return;
	}

	gui_test_exit_code = passed ? 0 : (exit_code == 0 ? 1 : exit_code);
	ScheduleGuiTestClose();
}

void AegisubApp::ScheduleGuiTestClose() {
	if (gui_test_close_scheduled)
		return;
	gui_test_close_scheduled = true;
	CallAfter([this] {
		CallAfter([this] {
			gui_test_close_scheduled = false;
			CloseAll();
		});
	});
}

int AegisubApp::OnExit() {
	auto record_exit_phase = [&](char const* phase) {
		if (!automation_profile)
			return;
		try {
			std::ofstream out(automation_profile->ArtifactsDirectory() / agi::fs::PathFromString("startup.log"), std::ios::app);
			out << phase << "\n";
		}
		catch (...) {
		}
	};
	record_exit_phase("exit.begin");
	gui_test_close_scheduled = false;
	record_exit_phase("exit.close-barrier-reset");
	ui_activation.Deactivate();
	record_exit_phase("exit.ui-deactivated");

	for (auto frame : frames)
		delete frame;
	frames.clear();
	record_exit_phase("exit.frames-cleared");

	if (wxTheClipboard->Open()) {
		wxTheClipboard->Flush();
		wxTheClipboard->Close();
	}
	record_exit_phase("exit.clipboard-flushed");

	runtime.reset();
	record_exit_phase("exit.runtime-reset");
	if (automation_profile) {
		automation_profile->Complete(gui_test_exit_code.value_or(0) == 0);
		record_exit_phase("exit.profile-complete");
	}

	auto result = wxApp::OnExit();
	record_exit_phase("exit.wx-complete");
	automation_profile.reset();
	return result;
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
			if (launch_plan && launch_plan->mode == AppLaunchMode::GuiTest && !gui_test_exit_code)
				gui_test_exit_code = 0;
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
	agi::fs::path last_recovered_path;
	for (auto& frame : frames) {
		auto c = frame->context.get();
		if (!c) continue;

		auto core = c->GetCore();
		if (!core.ass || !core.subsController) continue;

		try {
			path = config::path->Decode("?user/recovered");
			agi::fs::CreateDirectory(path);

			auto filename = core.subsController->Filename().stem();
			filename.replace_extension(agi::format("%s.ass", agi::util::strftime("%Y-%m-%d-%H-%M-%S")));
			path /= filename;
			core.subsController->Save(path);
			any = true;
			last_recovered_path = path;
		}
		catch (agi::Exception const& err) {
			crash_writer::Write("Crash recovery save failed: " + err.GetMessage());
		}
		catch (std::exception const& err) {
			crash_writer::Write(std::string("Crash recovery save failed: ") + err.what());
		}
		catch (...) {
			crash_writer::Write("Crash recovery save failed: unknown error");
		}
	}

	if (stackWalk)
		crash_writer::Write();

	if (any) {
		// Inform user of crash.
		ShowGuiWxBootstrapUiError(from_wx(_("Program error")), agi::format(exception_message, last_recovered_path));
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
		auto const result = MainLoop();
		return gui_test_exit_code.value_or(result);
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
		files.push_back(PathFromUtf8(from_wx(filenames[i])));
	if (!files.empty())
		frames[0]->context->GetCore().project->LoadList(files);
}
