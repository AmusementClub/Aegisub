#pragma once

#include <libaegisub/cajun/elements.h>
#include <libaegisub/fs_fwd.h>

#include <map>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegisub::automation_scenario {

struct Scenario {
	int version = 0;
	std::string name;
	std::vector<std::string> hosts;
	std::map<std::string, agi::fs::path> resources;
	/// Per-step wall clock for the process supervisor (not startup/teardown).
	/// Cold CoreCLR / plugin-bridge runs need a generous default; pure-Lua
	/// scenarios that should fail fast can set a shorter value explicitly.
	int default_timeout_ms = 120000;
	std::deque<json::Object> steps;
	agi::fs::path source_path;
};

struct LoadResult {
	std::optional<Scenario> scenario;
	std::string error;
};

using InputOverrides = std::vector<std::pair<std::string, std::string>>;

LoadResult Load(
	agi::fs::path const& path,
	InputOverrides const& input_overrides);

bool SupportsHost(Scenario const& scenario, std::string const& host);

}
