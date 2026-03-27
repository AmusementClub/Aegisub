#pragma once

#include "headless_playback_probe.h"

#include <utility>

namespace aegisub::playback_probe_service {

using PlaybackProbeRequest = headless_playback_probe::PlaybackProbeRequest;
using PlaybackProbeResult = headless_playback_probe::PlaybackProbeResult;

inline void RunAsync(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done) {
	headless_playback_probe::RunAsync(std::move(request), std::move(on_done));
}

}
