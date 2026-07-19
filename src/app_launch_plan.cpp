#include "app_launch_plan.h"

#include <libaegisub/path.h>

AppLaunchPlan ParseAppLaunchPlan(std::vector<std::string> const& args) {
	AppLaunchPlan plan;
	plan.original_args = args;
	if (args.size() < 2)
		return plan;

	if (args[1] == "--headless") {
		plan.mode = AppLaunchMode::Headless;
		if (args.size() >= 3 && (args[2] == "probe" || args[2] == "inspect")) {
			plan.legacy_headless_args = args;
			plan.legacy_headless_args[1] = "--cli";
			return plan;
		}
		auto parsed = aegisub::headless_automation_cli::Parse(args);
		if (parsed.requested) {
			plan.headless_run = std::move(parsed.request);
			plan.error = std::move(parsed.error);
			return plan;
		}
		plan.error = "--headless was not recognized by the automation parser";
		return plan;
	}

	if (args[1] == "--gui-test") {
		plan.mode = AppLaunchMode::GuiTest;
		if (args.size() < 3 || args[2] == "host") {
			plan.gui_test_host = true;
			aegisub::headless_automation_cli::RunRequest request;
			for (size_t i = 3; i < args.size(); ++i) {
				auto require_value = [&](std::string const& option) -> std::optional<std::string> {
					if (++i >= args.size()) {
						plan.error = option + " requires a value";
						return std::nullopt;
					}
					return args[i];
				};
				if (args[i] == "--profile-dir") {
					auto value = require_value(args[i]);
					if (!value) return plan;
					request.profile_directory = agi::fs::PathFromString(*value);
				}
				else if (args[i] == "--artifacts") {
					auto value = require_value(args[i]);
					if (!value) return plan;
					request.artifacts_directory = agi::fs::PathFromString(*value);
				}
				else if (args[i] == "--keep-profile") {
					request.keep_profile = true;
				}
				else if (args[i] == "--open") {
					auto value = require_value(args[i]);
					if (!value) return plan;
					plan.gui_test_open_files.push_back(*value);
				}
				else {
					plan.error = "unrecognized --gui-test host argument: " + args[i];
					return plan;
				}
			}
			plan.gui_test_run.emplace(std::move(request));
			return plan;
		}
		if (args[2] != "run") {
			plan.error = "--gui-test requires 'host' or 'run'";
			return plan;
		}
		auto translated = args;
		translated[1] = "--headless";
		translated[2] = "run";
		auto parsed = aegisub::headless_automation_cli::Parse(translated);
		plan.gui_test_run = std::move(parsed.request);
		plan.error = std::move(parsed.error);
		return plan;
	}

	// Legacy probe/inspect/session forms remain classified as headless until
	// their narrow interfaces are migrated to the versioned scenario runner.
	if (args[1] == "--cli" || args[1] == "--headless-playback-probe")
		plan.mode = AppLaunchMode::Headless;
	return plan;
}
