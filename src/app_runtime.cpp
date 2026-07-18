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
#include "app_runtime_facilities.h"
#include "app_runtime_init.h"
#include "ui_timer.h"

#include "command/command.h"
#include "include/aegisub/hotkey.h"

#include "options.h"
#include "perf_trace.h"
#include "version.h"

#include <libaegisub/exception.h>
#include <libaegisub/format.h>
#include <libaegisub/util.h>

#include <boost/locale.hpp>

#include <chrono>
#include <clocale>
#include <cstdlib>
#include <locale>
#include <utility>

namespace {

RuntimeShellMode current_shell_mode = RuntimeShellMode::Unknown;
bool runtime_commands_initialized = false;

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

void InitializeCommandsAndLocale(AppRuntimeInitOptions const& options, AegisubLocale& locale) {
	agi::util::SetThreadName("AegiMain");
	srand(time(nullptr));
	setlocale(LC_NUMERIC, "C");
	setlocale(LC_CTYPE, "C");
	OPT_SET("Version/Last Version")->SetInt(GetSVNRevision());

	if (options.initialize_commands) {
		cmd::init_builtin_commands();
		hotkey::init();
		runtime_commands_initialized = true;
	}

	if (!options.initialize_ui_locale)
		return;

	auto lang = OPT_GET("App/Language")->GetString();
	bool const has_language = !lang.empty() && (lang == "en_US" || locale.HasLanguage(lang));
	if (!has_language) {
		if (options.locale_policy == RuntimeLocalePolicy::PickIfNeeded)
			lang = locale.PickLanguage(options.bootstrap_ui_host.single_choice_sink);
		if (lang.empty() || !locale.HasLanguage(lang))
			lang = "en_US";
		OPT_SET("App/Language")->SetString(lang);
	}

	locale.Init(lang);
}

void CleanupRuntime() {
	if (runtime_commands_initialized) {
		hotkey::clear();
		cmd::clear();
		runtime_commands_initialized = false;
	}

	ShutdownRuntimeApplicationServices();
	ResetUiTimerHost();
	agi::dispatch::Shutdown();
	CleanupRuntimeProcessState();
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
			auto phase_started = std::chrono::steady_clock::now();
			auto finish_phase = [&]() {
				auto const now = std::chrono::steady_clock::now();
				auto const elapsed_ms = std::chrono::duration<double, std::milli>(now - phase_started).count();
				phase_started = now;
				return elapsed_ms;
			};

			if (options.process_host.prime_process_logging)
				options.process_host.prime_process_logging();
			auto const prime_process_logging_ms = finish_phase();
			InitializeGlobalLocale();
			auto const global_locale_ms = finish_phase();

			InstallUiTimerHost(options.ui_timer_host);
			auto const install_ui_timer_host_ms = finish_phase();

			agi::dispatch::Init(
				options.main_queue_hooks.invoke_main,
				options.main_queue_hooks.is_main_thread,
				options.main_queue_hooks.flush_main_jobs);
			auto const dispatch_init_ms = finish_phase();

			current_shell_mode = options.shell_mode;

			InitializeRuntimePathsAndOptions();
			auto const paths_and_options_ms = finish_phase();
			InitializeRuntimeLoggingAndPerfTrace();
			auto const logging_and_perf_trace_ms = finish_phase();
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.prime_process_logging", prime_process_logging_ms);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.global_locale", global_locale_ms);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.install_ui_timer_host", install_ui_timer_host_ms);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.dispatch.init", dispatch_init_ms);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.paths_and_options", paths_and_options_ms);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.logging_and_perf_trace", logging_and_perf_trace_ms);
			try {
				config::opt->ConfigUser();
			}
			catch (agi::Exception const& err) {
				ShowBootstrapUiError(
					options.bootstrap_ui_host,
					"Error",
					agi::format("Configuration file is invalid. Error reported:\n%s", err.GetMessage()));
			}
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.config_user", finish_phase());
			locale.SetHost(options.locale_host);
			InitializeCommandsAndLocale(options, locale);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.commands_and_locale", finish_phase());
			InitializeRuntimeOptionalFacilities(options);
			perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.optional_facilities", finish_phase());

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
