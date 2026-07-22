#pragma once

#include "headless_automation_cli.h"
#include "fontcollector_subcommand.h"
#include "headless_service_cli.h"

#include <string>
#include <optional>
#include <utility>
#include <vector>

enum class AppLaunchMode {
	Gui,
	Headless,
	GuiTest,
	FontCollector,
};

struct AppLaunchPlan {
	AppLaunchMode mode = AppLaunchMode::Gui;
	std::vector<std::string> original_args;
	std::optional<aegisub::headless_automation_cli::RunRequest> headless_run;
	std::optional<aegisub::headless_service_cli::Command> headless_service;
	std::optional<aegisub::headless_automation_cli::RunRequest> gui_test_run;
	std::optional<aegisub::fontcollector_subcommand::Options> fontcollector_options;
	bool gui_test_host = false;
	std::vector<std::string> gui_test_open_files;
	std::optional<int> immediate_exit_code;
	std::string immediate_output;
	std::string error;

	bool RequestedHeadless() const noexcept { return mode == AppLaunchMode::Headless; }
	bool RequestedFontCollector() const noexcept { return mode == AppLaunchMode::FontCollector; }
	bool RequestedPlainProcess() const noexcept {
		return RequestedHeadless() || RequestedFontCollector() || immediate_exit_code.has_value();
	}
	bool ParseSucceeded() const noexcept { return error.empty(); }
};

AppLaunchPlan ParseAppLaunchPlan(std::vector<std::string> const& args);
