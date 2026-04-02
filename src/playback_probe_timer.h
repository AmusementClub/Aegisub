#pragma once

#include <functional>
#include <memory>
#include <optional>

namespace aegisub::playback_probe_service {

class PlaybackProbeTimer {
public:
	virtual ~PlaybackProbeTimer() = default;

	virtual void StopAll() = 0;
	virtual void StartTimeoutOnce(int timeout_ms) = 0;
	virtual void StartCompletionPolling(int interval_ms) = 0;
	virtual void StartRestartOnce(int delay_ms) = 0;
	virtual void ArmSeek(std::optional<int> delay_ms) = 0;
};

std::unique_ptr<PlaybackProbeTimer> CreatePlaybackProbeTimer(
	std::function<void()> on_timeout,
	std::function<void()> on_completion_poll,
	std::function<void()> on_restart_timer,
	std::function<void()> on_seek_timer);

}
