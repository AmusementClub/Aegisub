#pragma once

#include "headless_playback_probe.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace headless_cli {

struct TraceInspectRequest {
	agi::fs::path input_path;
};

struct TraceInspectResult {
	int exit_code = 0;
	std::string output;
	std::string error;
};

struct BatchPlaybackProbeRequest {
	agi::fs::path list_file;
	agi::fs::path output_dir;
	headless_playback_probe::PlaybackProbeRequest probe_template;
};

struct BatchPlaybackProbeResult {
	int exit_code = 0;
	size_t total_cases = 0;
	size_t passed_cases = 0;
	size_t failed_cases = 0;
	agi::fs::path output_dir;
	std::string message;
};

struct ProbePlaybackCommand {
	headless_playback_probe::PlaybackProbeRequest request;
};

struct InspectTraceCommand {
	TraceInspectRequest request;
};

struct BatchPlaybackProbeCommand {
	BatchPlaybackProbeRequest request;
};

using Command = std::variant<ProbePlaybackCommand, InspectTraceCommand, BatchPlaybackProbeCommand>;

struct ParseResult {
	bool requested = false;
	std::optional<Command> command;
	std::string error;
};

ParseResult ParseCommandLine(std::vector<std::string> const& args);
TraceInspectResult RunInspectTrace(TraceInspectRequest const& request);
void RunBatchPlaybackProbeAsync(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done);
std::string Usage();

}
