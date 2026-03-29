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

struct AppRuntimeInitOptions {
	RuntimeShellMode shell_mode = RuntimeShellMode::Unknown;
	RuntimeLocalePolicy locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
	AppRuntimeMainQueueHooks main_queue_hooks;
	bool load_global_scripts = false;
	bool install_png_handler = true;
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
