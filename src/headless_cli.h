#pragma once

#include "headless_playback_probe.h"
#include "playback_session_service.h"
#include "trace_inspect_service.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace headless_cli {

using TraceInspectRequest = aegisub::trace_inspect_service::TraceInspectRequest;
using PlaybackSessionRequest = aegisub::playback_session_service::PlaybackSessionRequest;
using PlaybackSessionResult = aegisub::playback_session_service::PlaybackSessionResult;

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

struct SessionPlaybackCommand {
	PlaybackSessionRequest request;
};

struct InspectTraceCommand {
	TraceInspectRequest request;
};

struct BatchPlaybackProbeCommand {
	BatchPlaybackProbeRequest request;
};

using Command = std::variant<ProbePlaybackCommand, SessionPlaybackCommand, InspectTraceCommand, BatchPlaybackProbeCommand>;

struct ParseResult {
	bool requested = false;
	std::optional<Command> command;
	std::string error;
};

ParseResult ParseCommandLine(std::vector<std::string> const& args);
TraceInspectResult RunInspectTrace(TraceInspectRequest const& request);
void RunSessionPlaybackAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done);
void RunBatchPlaybackProbeAsync(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done);
std::string Usage();

}
