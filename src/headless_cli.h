#pragma once

#include "ass_info_service.h"
#include "headless_playback_probe.h"
#include "media_inspect_service.h"
#include "playback_session_service.h"
#include "trace_summary_service.h"
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
using MediaInspectRequest = aegisub::media_inspect_service::MediaInspectRequest;
using MediaInspectResult = aegisub::media_inspect_service::MediaInspectResult;
using AssInfoInspectRequest = aegisub::ass_info_service::AssInfoInspectRequest;
using AssInfoInspectResult = aegisub::ass_info_service::AssInfoInspectResult;

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

struct InspectMediaCommand {
	MediaInspectRequest request;
};

struct InspectAssInfoCommand {
	AssInfoInspectRequest request;
};

struct InspectTraceCommand {
	TraceInspectRequest request;
};

struct BatchPlaybackProbeCommand {
	BatchPlaybackProbeRequest request;
};

struct BatchTraceSummarizeRequest {
	std::vector<agi::fs::path> inputs;
	agi::fs::path output_dir;
};

struct BatchTraceSummarizeResult {
	int exit_code = 0;
	size_t total_sessions = 0;
	size_t passed_sessions = 0;
	size_t failed_sessions = 0;
	agi::fs::path output_dir;
	std::string message;
};

struct BatchTraceSummarizeCommand {
	BatchTraceSummarizeRequest request;
};

struct BatchAssInfoRequest {
	std::vector<agi::fs::path> inputs;
	agi::fs::path output_dir;
	std::string encoding;
};

struct BatchAssInfoResult {
	int exit_code = 0;
	size_t total_files = 0;
	size_t passed_files = 0;
	size_t failed_files = 0;
	agi::fs::path output_dir;
	std::string message;
};

struct BatchAssInfoCommand {
	BatchAssInfoRequest request;
};

using Command = std::variant<
	ProbePlaybackCommand,
	SessionPlaybackCommand,
	InspectMediaCommand,
	InspectAssInfoCommand,
	InspectTraceCommand,
	BatchPlaybackProbeCommand,
	BatchTraceSummarizeCommand,
	BatchAssInfoCommand>;

struct ParseResult {
	bool requested = false;
	std::optional<Command> command;
	std::string error;
};

ParseResult ParseCommandLine(std::vector<std::string> const& args);
TraceInspectResult RunInspectTrace(TraceInspectRequest const& request);
MediaInspectResult RunInspectMedia(MediaInspectRequest const& request);
AssInfoInspectResult RunInspectAssInfo(AssInfoInspectRequest const& request);
std::string BuildMediaInspectJson(MediaInspectResult const& result);
std::string BuildAssInfoJson(AssInfoInspectResult const& result);
void RunSessionPlaybackAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done);
void RunBatchPlaybackProbeAsync(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done);
BatchTraceSummarizeResult RunBatchTraceSummarize(BatchTraceSummarizeRequest const& request);
BatchAssInfoResult RunBatchAssInfo(BatchAssInfoRequest const& request);
std::string Usage();

}
