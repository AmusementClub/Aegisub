#pragma once

#include <functional>
#include <memory>

class VideoControllerTimerHost {
public:
	virtual ~VideoControllerTimerHost() = default;

	virtual void Start(int interval_ms) = 0;
	virtual void Stop() = 0;
	virtual bool IsRunning() const = 0;
};

std::unique_ptr<VideoControllerTimerHost> CreateVideoControllerTimerHost(
	std::function<void()> on_play_timer);
