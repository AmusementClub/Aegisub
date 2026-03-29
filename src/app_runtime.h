#pragma once

#include "aegisublocale.h"

#include <libaegisub/dispatch.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace agi { class SingleChoiceInteractionSink; }

enum class RuntimeShellMode {
	Unknown,
	Gui,
	Headless,
};

enum class RuntimeLocalePolicy {
	PickIfNeeded,
	UseConfiguredOrEnglish,
};

struct AppRuntimeMainQueueHooks {
	std::function<void(agi::dispatch::Thunk)> invoke_main;
	std::function<bool()> is_main_thread;
	std::function<std::size_t()> flush_main_jobs;
};

struct AppRuntimeHostHooks {
	std::function<void()> prime_process_logging;
	std::function<void()> install_png_image_handler;
};

struct AppRuntimeInitOptions {
	RuntimeShellMode shell_mode = RuntimeShellMode::Unknown;
	RuntimeLocalePolicy locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
	AppRuntimeMainQueueHooks main_queue_hooks;
	bool load_global_scripts = false;
	bool initialize_commands = true;
	bool initialize_ui_locale = true;
	bool register_automation_script_factory = true;
	bool warm_subtitles_provider_font_cache = true;
	bool register_export_filters = true;
	bool install_png_handler = true;
	AppRuntimeHostHooks host_hooks;
	std::shared_ptr<agi::SingleChoiceInteractionSink> single_choice_sink;
	std::function<void(std::string const& title, std::string const& message)> report_nonfatal_error;
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
