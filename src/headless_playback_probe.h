#pragma once

#include "playback_probe_service.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace headless_playback_probe {

using PlaybackProbeRequest = aegisub::playback_probe_service::PlaybackProbeRequest;
using PlaybackProbeResult = aegisub::playback_probe_service::PlaybackProbeResult;

struct CommandLineParseResult {
	bool requested = false;
	std::optional<PlaybackProbeRequest> request;
	std::string error;
};

struct RequestParseResult {
	std::optional<PlaybackProbeRequest> request;
	std::string error;
};

RequestParseResult ParseRequestArguments(std::vector<std::string> const& args, bool require_video);
CommandLineParseResult ParseCommandLine(std::vector<std::string> const& args);
void RunAsync(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done);
std::string Usage();

}
