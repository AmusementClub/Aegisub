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
#include "auto4_lua_factory.h"
#include "compat.h"
#include "crash_writer.h"
#include "dialogs.h"
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "format.h"
#include "frame_main.h"
#include "headless_cli.h"
#include "headless_playback_probe.h"
#include "playback_probe_service.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "subs_controller.h"
#include "subtitles_provider_libass.h"
#include "utils.h"
#include "value_event.h"
#include "version.h"
#include "wx_ui_services.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/locale.hpp>
#include <iostream>
#include <locale>
#include <vector>
#include <wx/arrstr.h>
#include <wx/clipbrd.h>
#include <wx/msgdlg.h>
#include <wx/stackwalk.h>
#include <wx/thread.h>
#include <wx/utils.h>

namespace config {
	agi::Options *opt = nullptr;
	agi::MRUManager *mru = nullptr;
	agi::Path *path = nullptr;
	Automation4::AutoloadScriptManager *global_scripts;
}

wxIMPLEMENT_APP(AegisubApp);

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
wxDEFINE_EVENT(EVT_CALL_THUNK, ValueEvent<agi::dispatch::Thunk>);

std::vector<std::string> ToUtf8Args(wxArrayString const& args) {
	std::vector<std::string> values;
	values.reserve(args.size());
	for (auto const& arg : args)
		values.emplace_back(arg.ToStdString(wxConvUTF8));
	return values;
}

agi::WxMessageBoxNotificationSink& AppNotificationSink() {
	static agi::WxMessageBoxNotificationSink sink(nullptr);
	return sink;
}

agi::WxMessageBoxInteractionSink& AppInteractionSink() {
	static agi::WxMessageBoxInteractionSink sink(nullptr);
	return sink;
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

	// The logger isn't created on demand on background threads, so force it to
	// be created now
	(void)wxLog::GetActiveTarget();

	{
		// Try to get the UTF-8 version of the current locale
		auto locale = boost::locale::generator().generate("");

		// Check if we actually got a UTF-8 locale
		using codecvt = std::codecvt<wchar_t, char, std::mbstate_t>;
		int result = std::codecvt_base::error;
		if (std::has_facet<codecvt>(locale)) {
			wchar_t test[] = L"\xFFFE";
			char buff[8];
			auto mb = std::mbstate_t();
			const wchar_t* from_next;
			char* to_next;
			result = std::use_facet<codecvt>(locale).out(mb,
				test, std::end(test), from_next,
				buff, std::end(buff), to_next);
		}

		// If we didn't get a UTF-8 locale, force it to a known one
		if (result != std::codecvt_base::ok)
			locale = boost::locale::generator().generate("en_US.UTF-8");
		std::locale::global(locale);
	}

#if defined(__GNUC__) && (__GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8))
	// Pointless `this` capture required due to http://gcc.gnu.org/bugzilla/show_bug.cgi?id=51494
	agi::dispatch::Init([this](agi::dispatch::Thunk f) {
#else
	agi::dispatch::Init([](agi::dispatch::Thunk f) {
#endif
		auto evt = new ValueEvent<agi::dispatch::Thunk>(EVT_CALL_THUNK, -1, std::move(f));
		wxTheApp->QueueEvent(evt);
	}, [] {
		return wxIsMainThread();
	});

	wxTheApp->Bind(EVT_CALL_THUNK, [this](ValueEvent<agi::dispatch::Thunk>& evt) {
		try {
			evt.Get()();
		}
		catch (...) {
			OnExceptionInMainLoop();
		}
	});

	config::path = new agi::Path;
	crash_writer::Initialize(config::path->Decode("?user"));

	agi::log::log = new agi::log::LogSink;
#ifdef _DEBUG
	agi::log::log->Subscribe(agi::make_unique<agi::log::EmitSTDOUT>());
#endif

	// Set config file
	StartupLog("Load local configuration");
#ifdef __WXMSW__
	// Try loading configuration from the install dir if one exists there
	try {
		auto conf_local(config::path->Decode("?data/config.json"));
		std::unique_ptr<std::istream> localConfig(agi::io::Open(conf_local));
		config::opt = new agi::Options(conf_local, GET_DEFAULT_CONFIG(default_config));

		// Local config, make ?user mean ?data so all user settings are placed in install dir
		config::path->SetToken("?user", config::path->Decode("?data"));
		config::path->SetToken("?local", config::path->Decode("?data"));
		crash_writer::Initialize(config::path->Decode("?user"));
	} catch (agi::fs::FileSystemError const&) {
		// File doesn't exist or we can't read it
		// Might be worth displaying an error in the second case
	}
#endif

	StartupLog("Create log writer");
	perf_trace::Initialize(GetAegisubLongVersionString());
	auto path_log = config::path->Decode("?user/log/");
	agi::fs::CreateDirectory(path_log);
	agi::log::log->Subscribe(agi::make_unique<agi::log::JsonEmitter>(path_log));
	CleanCache(path_log, "*.ndjson", 10, 100);
	CleanCache(path_log, "*.json", 10, 100);

	StartupLog("Load user configuration");
	try {
		if (!config::opt)
			config::opt = new agi::Options(config::path->Decode("?user/config.json"), GET_DEFAULT_CONFIG(default_config));
		boost::interprocess::ibufferstream stream((const char *)default_config_platform, sizeof(default_config_platform));
		config::opt->ConfigNext(stream);
	} catch (agi::Exception& e) {
		LOG_E("config/init") << "Caught exception: " << e.GetMessage();
	}

	try {
		config::opt->ConfigUser();
	}
	catch (agi::Exception const& err) {
		AppNotificationSink().ShowError("Error",
			agi::format("Configuration file is invalid. Error reported:\n%s", err.GetMessage()));
	}

#ifdef _WIN32
	StartupLog("Load installer configuration");
	if (OPT_GET("App/First Start")->GetBool()) {
		try {
			auto installer_config = agi::io::Open(config::path->Decode("?data/installer_config.json"));
			config::opt->ConfigNext(*installer_config.get());
		} catch (agi::fs::FileSystemError const&) {
			// Not an error obviously as the user may not have used the installer
		}
	}
#endif

	// Init commands.
	cmd::init_builtin_commands();

	// Init hotkeys
	hotkey::init();

	StartupLog("Load MRU");
	config::mru = new agi::MRUManager(config::path->Decode("?user/mru.json"), GET_DEFAULT_CONFIG(default_mru), config::opt);

	agi::util::SetThreadName("AegiMain");

	StartupLog("Inside OnInit");
	try {
		// Initialize randomizer
		StartupLog("Initialize random generator");
		srand(time(nullptr));

		// locale for loading options
		StartupLog("Set initial locale");
		setlocale(LC_NUMERIC, "C");
		setlocale(LC_CTYPE, "C");

		// Crash handling
#if (!defined(_DEBUG) || defined(WITH_EXCEPTIONS)) && (wxUSE_ON_FATAL_EXCEPTION+0)
		StartupLog("Install exception handler");
		wxHandleFatalExceptions(true);
#endif

		StartupLog("Store options back");
		OPT_SET("Version/Last Version")->SetInt(GetSVNRevision());

		StartupLog("Initialize final locale");

		// Set locale
		auto lang = OPT_GET("App/Language")->GetString();
		if (lang.empty() || (lang != "en_US" && !locale.HasLanguage(lang))) {
			lang = locale.PickLanguage();
			OPT_SET("App/Language")->SetString(lang);
		}
		locale.Init(lang);

#ifdef __APPLE__
		// When run from an app bundle, LC_CTYPE defaults to "C", which breaks on
		// anything involving unicode and in some cases number formatting.
		// The right thing to do here would be to query CoreFoundation for the user's
		// locale and add .UTF-8 to that, but :effort:
		setlocale(LC_CTYPE, "en_US.UTF-8");
#endif

		exception_message = _("Oops, Aegisub has crashed!\n\nAn attempt has been made to save a copy of your file to:\n\n%s\n\nAegisub will now close.");

		// Load plugins
		Automation4::ScriptFactory::Register(agi::make_unique<Automation4::LuaScriptFactory>());
		libass::CacheFonts();

		// Load Automation scripts
		StartupLog("Load global Automation scripts");
		config::global_scripts = new Automation4::AutoloadScriptManager(OPT_GET("Path/Automation/Autoload")->GetString());

		// Load export filters
		StartupLog("Register export filters");
		AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
		AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());

		StartupLog("Install PNG handler");
		wxImage::AddHandler(new wxPNGHandler);

		auto cli_parse = headless_cli::ParseCommandLine(ToUtf8Args(argv.GetArguments()));
		if (cli_parse.requested) {
			headless_cli_mode = true;
			if (!cli_parse.command) {
				headless_cli_exit_code = 64;
				std::cerr << cli_parse.error << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			if (auto *probe = std::get_if<headless_cli::ProbePlaybackCommand>(&*cli_parse.command)) {
				auto probe_request = probe->request;
				CallAfter([this, probe_request = std::move(probe_request)]() mutable {
					aegisub::playback_probe_service::RunAsync(std::move(probe_request), [this](headless_playback_probe::PlaybackProbeResult result) {
						headless_cli_exit_code = result.exit_code;
						ExitMainLoop();
					});
				});
				return true;
			}

			if (auto *session = std::get_if<headless_cli::SessionPlaybackCommand>(&*cli_parse.command)) {
				auto session_request = session->request;
				CallAfter([this, session_request = std::move(session_request)]() mutable {
					headless_cli::RunSessionPlaybackAsync(std::move(session_request), [this](headless_cli::PlaybackSessionResult result) {
						headless_cli_exit_code = result.exit_code;
						ExitMainLoop();
					});
				});
				return true;
			}

			if (auto *inspect = std::get_if<headless_cli::InspectTraceCommand>(&*cli_parse.command)) {
				auto inspect_result = headless_cli::RunInspectTrace(inspect->request);
				headless_cli_exit_code = inspect_result.exit_code;
				if (!inspect_result.output.empty())
					std::cout << inspect_result.output;
				if (!inspect_result.error.empty())
					std::cerr << inspect_result.error << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			if (auto *inspect = std::get_if<headless_cli::InspectMediaCommand>(&*cli_parse.command)) {
				auto inspect_result = headless_cli::RunInspectMedia(inspect->request);
				headless_cli_exit_code = inspect_result.exit_code;
				std::cout << headless_cli::BuildMediaInspectJson(inspect_result);
				if (inspect_result.exit_code != 0 && !inspect_result.message.empty())
					std::cerr << inspect_result.message << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			if (auto *inspect = std::get_if<headless_cli::InspectAssInfoCommand>(&*cli_parse.command)) {
				auto inspect_result = headless_cli::RunInspectAssInfo(inspect->request);
				headless_cli_exit_code = inspect_result.snapshot ? 0 : 2;
				std::cout << headless_cli::BuildAssInfoJson(inspect_result);
				if (!inspect_result.snapshot && !inspect_result.error.empty())
					std::cerr << inspect_result.error << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			if (auto *batch = std::get_if<headless_cli::BatchPlaybackProbeCommand>(&*cli_parse.command)) {
				auto batch_request = batch->request;
				CallAfter([this, batch_request = std::move(batch_request)]() mutable {
					headless_cli::RunBatchPlaybackProbeAsync(std::move(batch_request), [this](headless_cli::BatchPlaybackProbeResult result) {
						headless_cli_exit_code = result.exit_code;
						ExitMainLoop();
					});
				});
				return true;
			}

			if (auto *batch = std::get_if<headless_cli::BatchTraceSummarizeCommand>(&*cli_parse.command)) {
				auto batch_result = headless_cli::RunBatchTraceSummarize(batch->request);
				headless_cli_exit_code = batch_result.exit_code;
				if (!batch_result.message.empty())
					std::cout << batch_result.message << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			if (auto *batch = std::get_if<headless_cli::BatchAssInfoCommand>(&*cli_parse.command)) {
				auto batch_result = headless_cli::RunBatchAssInfo(batch->request);
				headless_cli_exit_code = batch_result.exit_code;
				if (!batch_result.message.empty())
					std::cout << batch_result.message << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			headless_cli_exit_code = 64;
			std::cerr << "unhandled CLI command" << std::endl;
			CallAfter([this] { ExitMainLoop(); });
			return true;
		}

		auto probe_parse = headless_playback_probe::ParseCommandLine(ToUtf8Args(argv.GetArguments()));
		if (probe_parse.requested) {
			headless_cli_mode = true;
			if (!probe_parse.request) {
				headless_cli_exit_code = 64;
				std::cerr << probe_parse.error << std::endl;
				CallAfter([this] { ExitMainLoop(); });
				return true;
			}

			auto probe_request = *probe_parse.request;
			CallAfter([this, probe_request = std::move(probe_request)]() mutable {
				aegisub::playback_probe_service::RunAsync(std::move(probe_request), [this](headless_playback_probe::PlaybackProbeResult result) {
					headless_cli_exit_code = result.exit_code;
					ExitMainLoop();
				});
			});
			return true;
		}

		// Open main frame
		StartupLog("Create main window");
		NewProjectContext();

		// Version checker
		StartupLog("Possibly perform automatic updates check");
		if (OPT_GET("App/First Start")->GetBool()) {
			OPT_SET("App/First Start")->SetBool(false);
#ifdef WITH_UPDATE_CHECKER
			auto result = AppInteractionSink().Request({
				from_wx(_("Check for updates?")),
				from_wx(_("Do you want Aegisub to check for updates whenever it starts? You can still do it manually via the Help menu.")),
				agi::InteractionButtons::YesNo,
				agi::InteractionIcon::Question
			});
			OPT_SET("App/Auto/Check For Updates")->SetBool(result == agi::InteractionResult::Yes);
			try {
				config::opt->Flush();
			}
			catch (agi::fs::FileSystemError const& e) {
				AppNotificationSink().ShowError("Error saving config file", e.GetMessage());
			}
#endif
		}

#ifdef WITH_UPDATE_CHECKER
		PerformVersionCheck(false);
#endif

		// Get parameter subs
		StartupLog("Parse command line");
		auto const& args = argv.GetArguments();
		if (args.size() > 1)
			OpenFiles(wxArrayStringsAdapter(args.size() - 1, &args[1]));
	}
	catch (agi::Exception const& e) {
		AppNotificationSink().ShowError("Fatal error while initializing", e.GetMessage());
		return false;
	}
	catch (std::exception const& e) {
		AppNotificationSink().ShowError("Fatal error while initializing", e.what());
		return false;
	}
#ifndef _DEBUG
	catch (...) {
		AppNotificationSink().ShowError("Fatal error while initializing", "Unhandled exception");
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

	delete config::opt;
	delete config::mru;
	hotkey::clear();
	cmd::clear();

	delete config::global_scripts;

	AssExportFilterChain::Clear();

	perf_trace::Shutdown();

	// Keep this last!
	delete agi::log::log;
	crash_writer::Cleanup();

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
		AppNotificationSink().ShowError(from_wx(_("Program error")), agi::format(exception_message, path));
	}
	else if (LastStartupState) {
		AppNotificationSink().ShowError(from_wx(_("Program error")),
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
		AppNotificationSink().ShowError("Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), e.GetMessage()));
	}
	catch (const std::exception &e) {
		AppNotificationSink().ShowError("Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), e.what()));
	}
	catch (...) {
		AppNotificationSink().ShowError("Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), "Unknown error"));
	}
	return true;
}

int AegisubApp::OnRun() {
	std::string error;

	try {
		auto exit_code = MainLoop();
		if (headless_cli_mode)
			return headless_cli_exit_code;
		return exit_code;
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
