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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "app_runtime.h"

#include "command/command.h"
#include "include/aegisub/hotkey.h"

#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "crash_writer.h"
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "subtitles_provider_libass.h"
#include "utils.h"
#include "version.h"

#include <libaegisub/exception.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/locale.hpp>

#include <clocale>
#include <cstdlib>
#include <locale>
#include <memory>
#include <optional>
#include <utility>

// Shared runtime initialization stays at the process-shell boundary. It can
// touch minimal wx runtime facilities, but service/session code should consume
// the plain AppRuntime surface instead of depending on wx directly.
#include <wx/image.h>
#include <wx/log.h>

namespace {

RuntimeShellMode current_shell_mode = RuntimeShellMode::Unknown;

void InitializeGlobalLocale() {
	auto locale = boost::locale::generator().generate("");

	using codecvt = std::codecvt<wchar_t, char, std::mbstate_t>;
	int result = std::codecvt_base::error;
	if (std::has_facet<codecvt>(locale)) {
		wchar_t test[] = L"\xFFFE";
		char buff[8];
		auto mb = std::mbstate_t();
		const wchar_t* from_next;
		char* to_next;
		result = std::use_facet<codecvt>(locale).out(
			mb,
			test, std::end(test), from_next,
			buff, std::end(buff), to_next);
	}

	if (result != std::codecvt_base::ok)
		locale = boost::locale::generator().generate("en_US.UTF-8");
	std::locale::global(locale);
}

void InitializePathsAndOptions() {
	config::path = new agi::Path;
	crash_writer::Initialize(config::path->Decode("?user"));

	if (!agi::log::log) {
		agi::log::log = new agi::log::LogSink;
#ifdef _DEBUG
		agi::log::log->Subscribe(agi::make_unique<agi::log::EmitSTDOUT>());
#endif
	}

#ifdef __WXMSW__
	try {
		auto conf_local(config::path->Decode("?data/config.json"));
		std::unique_ptr<std::istream> local_config(agi::io::Open(conf_local));
		config::opt = new agi::Options(conf_local, GET_DEFAULT_CONFIG(default_config));
		config::path->SetToken("?user", config::path->Decode("?data"));
		config::path->SetToken("?local", config::path->Decode("?data"));
		crash_writer::Initialize(config::path->Decode("?user"));
	}
	catch (agi::fs::FileSystemError const&) {
	}
#endif

	if (!config::opt) {
		config::opt = new agi::Options(config::path->Decode("?user/config.json"), GET_DEFAULT_CONFIG(default_config));
	}

	boost::interprocess::ibufferstream stream((const char *)default_config_platform, sizeof(default_config_platform));
	config::opt->ConfigNext(stream);

#ifdef _WIN32
	if (OPT_GET("App/First Start")->GetBool()) {
		try {
			auto installer_config = agi::io::Open(config::path->Decode("?data/installer_config.json"));
			config::opt->ConfigNext(*installer_config.get());
		}
		catch (agi::fs::FileSystemError const&) {
		}
	}
#endif

	config::mru = new agi::MRUManager(config::path->Decode("?user/mru.json"), GET_DEFAULT_CONFIG(default_mru), config::opt);
}

void InitializeLoggingAndPerfTrace() {
	perf_trace::Initialize(GetAegisubLongVersionString());
	auto path_log = config::path->Decode("?user/log/");
	agi::fs::CreateDirectory(path_log);
	agi::log::log->Subscribe(agi::make_unique<agi::log::JsonEmitter>(path_log));
	CleanCache(path_log, "*.ndjson", 10, 100, 24 * 60 * 60);
	CleanCache(path_log, "*.json", 10, 100, 24 * 60 * 60);
}

void InitializeCommandsAndLocale(AppRuntimeInitOptions const& options, AegisubLocale& locale) {
	cmd::init_builtin_commands();
	hotkey::init();

	agi::util::SetThreadName("AegiMain");
	srand(time(nullptr));
	setlocale(LC_NUMERIC, "C");
	setlocale(LC_CTYPE, "C");
	OPT_SET("Version/Last Version")->SetInt(GetSVNRevision());

	auto lang = OPT_GET("App/Language")->GetString();
	bool const has_language = !lang.empty() && (lang == "en_US" || locale.HasLanguage(lang));
	if (!has_language) {
		if (options.locale_policy == RuntimeLocalePolicy::PickIfNeeded)
			lang = locale.PickLanguage(options.single_choice_sink);
		if (lang.empty() || !locale.HasLanguage(lang))
			lang = "en_US";
		OPT_SET("App/Language")->SetString(lang);
	}

	locale.Init(lang);
}

void InitializeAutomationAndFilters(AppRuntimeInitOptions const& options) {
	Automation4::ScriptFactory::Register(agi::make_unique<Automation4::LuaScriptFactory>());
	libass::CacheFonts();

	if (options.load_global_scripts)
		config::global_scripts = new Automation4::AutoloadScriptManager(OPT_GET("Path/Automation/Autoload")->GetString());

	AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
	AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());

	if (options.install_png_handler)
		wxImage::AddHandler(new wxPNGHandler);
}

void CleanupRuntime() {
	if (config::opt) {
		delete config::opt;
		config::opt = nullptr;
	}
	if (config::mru) {
		delete config::mru;
		config::mru = nullptr;
	}

	hotkey::clear();
	cmd::clear();

	if (config::global_scripts) {
		delete config::global_scripts;
		config::global_scripts = nullptr;
	}

	AssExportFilterChain::Clear();

	perf_trace::Shutdown();

	if (agi::log::log) {
		delete agi::log::log;
		agi::log::log = nullptr;
	}

	// Legacy startup/shutdown left config::path alive until process teardown.
	// Keep the same lifetime for now so late cleanup paths do not dereference
	// a freed process-global path object.

	crash_writer::Cleanup();
	current_shell_mode = RuntimeShellMode::Unknown;
}

}

class AppRuntime::Impl {
public:
	AppRuntimeInitOptions options;
	AegisubLocale locale;
	bool initialized = false;

	bool Initialize(AppRuntimeInitOptions init_options, std::string& error) {
		options = std::move(init_options);
		try {
			(void)wxLog::GetActiveTarget();
			InitializeGlobalLocale();

			agi::dispatch::Init(
				options.main_queue_hooks.invoke_main,
				options.main_queue_hooks.is_main_thread,
				options.main_queue_hooks.flush_main_jobs);

			current_shell_mode = options.shell_mode;

			InitializePathsAndOptions();
			InitializeLoggingAndPerfTrace();
			try {
				config::opt->ConfigUser();
			}
			catch (agi::Exception const& err) {
				if (options.report_nonfatal_error) {
					options.report_nonfatal_error(
						"Error",
						agi::format("Configuration file is invalid. Error reported:\n%s", err.GetMessage()));
				}
			}
			InitializeCommandsAndLocale(options, locale);
			InitializeAutomationAndFilters(options);

			initialized = true;
			return true;
		}
		catch (agi::Exception const& err) {
			error = err.GetMessage();
		}
		catch (std::exception const& err) {
			error = err.what();
		}
		catch (...) {
			error = "Unhandled exception during runtime initialization";
		}

		CleanupRuntime();
		initialized = false;
		return false;
	}

	void Shutdown() {
		if (!initialized)
			return;
		CleanupRuntime();
		initialized = false;
	}
};

RuntimeShellMode GetRuntimeShellMode() {
	return current_shell_mode;
}

bool IsGuiRuntimeShell() {
	return current_shell_mode == RuntimeShellMode::Gui;
}

AppRuntime::AppRuntime()
: impl(std::make_unique<Impl>()) {
}

AppRuntime::~AppRuntime() {
	Shutdown();
}

bool AppRuntime::Initialize(AppRuntimeInitOptions options, std::string& error) {
	return impl->Initialize(std::move(options), error);
}

void AppRuntime::Shutdown() {
	impl->Shutdown();
}

AegisubLocale& AppRuntime::Locale() {
	return impl->locale;
}

AegisubLocale const& AppRuntime::Locale() const {
	return impl->locale;
}

RuntimeShellMode AppRuntime::ShellMode() const {
	return impl->options.shell_mode;
}

bool AppRuntime::IsInitialized() const {
	return impl->initialized;
}
