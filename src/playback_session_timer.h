#pragma once

#include <functional>
#include <memory>

namespace aegisub::playback_session_service {

class PlaybackSessionTimer {
public:
	virtual ~PlaybackSessionTimer() = default;

	virtual void StopAll() = 0;
	virtual void StartDelayOnce(int delay_ms) = 0;
	virtual void StartWaitPolling(int interval_ms) = 0;
	virtual void StopWaitPolling() = 0;
};

std::unique_ptr<PlaybackSessionTimer> CreatePlaybackSessionTimer(
	std::function<void()> on_delay_timer,
	std::function<void()> on_wait_timer);

}
