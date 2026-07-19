// Test seam for deterministic supervisor state-machine tests.
// Production callers should include automation_process_supervisor.h instead.
#pragma once

#include "automation_process_supervisor.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace aegisub::automation_process_supervisor::testing {

struct Timeouts {
	std::chrono::milliseconds startup{30'000};
	std::chrono::milliseconds teardown{120'000};
	std::chrono::milliseconds exit{5'000};
};

struct Hooks {
	/// Start the supervised child. On false, set `error` and leave the process unstarted.
	std::function<bool(
		std::vector<std::string> const& arguments,
		agi::fs::path const& log_directory,
		std::string& error)> start;
	/// Return true when the child has exited; write its exit code.
	std::function<bool(int& exit_code)> poll;
	std::function<void()> terminate;
	std::function<std::chrono::steady_clock::time_point()> now;
	std::function<void(std::chrono::milliseconds)> sleep;
	Timeouts timeouts;
};

Result Run(
	std::vector<std::string> const& arguments,
	agi::fs::path const& control_directory,
	int step_timeout_ms,
	std::size_t step_count,
	Hooks hooks);

}
