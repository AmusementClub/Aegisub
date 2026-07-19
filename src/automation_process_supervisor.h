#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::automation_process_supervisor {

struct Result {
	bool started = false;
	bool timed_out = false;
	int exit_code = 2;
	std::optional<std::size_t> step_index;
	/// "start" | "startup" | "step" | "teardown" | "exit"
	std::string phase;
	std::string error;
};

Result Run(
	std::vector<std::string> const& arguments,
	agi::fs::path const& control_directory,
	int step_timeout_ms,
	std::size_t step_count);

void MarkRuntimeReady(agi::fs::path const& control_directory);
void MarkStep(agi::fs::path const& control_directory, std::size_t index, bool started);
/// Emitted after scenario execution returns (success or handled failure),
/// before HeadlessRuntimeEnvironment destruction / managed unload begins.
void MarkWorkerScenarioComplete(agi::fs::path const& control_directory);
/// Emitted after the worker tears down the runtime and writes result.json.
void MarkWorkerShutdownComplete(agi::fs::path const& control_directory);

}
