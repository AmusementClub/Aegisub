#pragma once

#include "automation_scenario.h"

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>
#include <vector>

namespace aegisub::headless_automation_cli {

struct RunRequest {
	agi::fs::path scenario_path;
	automation_scenario::InputOverrides inputs;
	std::optional<agi::fs::path> profile_directory;
	std::optional<agi::fs::path> artifacts_directory;
	bool keep_profile = false;
	bool internal_worker = false;
};

}
