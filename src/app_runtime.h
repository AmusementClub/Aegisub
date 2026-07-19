#pragma once

#include "aegisublocale.h"
#include "runtime_optional_facility_host.h"
#include "runtime_process_host.h"
#include "runtime_bootstrap_ui_host.h"
#include "runtime_locale_host.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

class UiTimerHost;

enum class RuntimeShellMode {
	Unknown,
	Gui,
	Headless,
};

enum class RuntimeLocalePolicy {
	PickIfNeeded,
	UseConfiguredOrEnglish,
};

struct RuntimePathOverrides {
	std::shared_ptr<agi::fs::path> user_directory;
	std::shared_ptr<agi::fs::path> local_directory;
	bool allow_portable_config = true;
};

struct AppRuntimeMainQueueHooks {
	std::function<void(agi::dispatch::Thunk)> invoke_main;
	std::function<bool()> is_main_thread;
	std::function<std::size_t()> flush_main_jobs;
};

struct AppRuntimeInitOptions {
	RuntimeShellMode shell_mode = RuntimeShellMode::Unknown;
	RuntimeLocalePolicy locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
	RuntimePathOverrides path_overrides;
	AppRuntimeMainQueueHooks main_queue_hooks;
	RuntimeLocaleHost locale_host;
	bool load_global_scripts = false;
	bool initialize_commands = true;
	bool initialize_ui_locale = true;
	bool register_automation_script_factory = true;
	bool warm_subtitles_provider_font_cache = true;
	bool register_export_filters = true;
	bool install_png_handler = true;
	std::shared_ptr<UiTimerHost> ui_timer_host;
	RuntimeProcessHost process_host;
	RuntimeOptionalFacilityHost optional_facility_host;
	RuntimeBootstrapUiHost bootstrap_ui_host;
};

RuntimeShellMode GetRuntimeShellMode();
bool IsGuiRuntimeShell();

class AppRuntime {
	class Impl;
	std::unique_ptr<Impl> impl;

public:
	AppRuntime();
	~AppRuntime();

	AppRuntime(AppRuntime const&) = delete;
	AppRuntime& operator=(AppRuntime const&) = delete;
	AppRuntime(AppRuntime&&) = delete;
	AppRuntime& operator=(AppRuntime&&) = delete;

	bool Initialize(AppRuntimeInitOptions options, std::string& error);
	void Shutdown();

	AegisubLocale& Locale();
	AegisubLocale const& Locale() const;
	RuntimeShellMode ShellMode() const;
	bool IsInitialized() const;
};
