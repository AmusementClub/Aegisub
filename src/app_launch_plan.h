#pragma once

#include "headless_automation_cli.h"

#include <string>
#include <optional>
#include <utility>
#include <vector>

enum class AppLaunchMode {
	Gui,
	Headless,
	GuiTest,
};

struct AppLaunchPlan {
	AppLaunchMode mode = AppLaunchMode::Gui;
	std::vector<std::string> original_args;
	std::optional<aegisub::headless_automation_cli::RunRequest> headless_run;
	std::optional<aegisub::headless_automation_cli::RunRequest> gui_test_run;
	std::vector<std::string> legacy_headless_args;
	bool gui_test_host = false;
	std::vector<std::string> gui_test_open_files;
	std::string error;

	bool RequestedHeadless() const noexcept { return mode == AppLaunchMode::Headless; }
	bool ParseSucceeded() const noexcept { return error.empty(); }
};

AppLaunchPlan ParseAppLaunchPlan(std::vector<std::string> const& args);
