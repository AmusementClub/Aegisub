#pragma once

#include <functional>
#include <memory>

class VideoControllerTimer {
public:
	virtual ~VideoControllerTimer() = default;

	virtual void StartOnce(int delay_ms) = 0;
	virtual void Start(int interval_ms) = 0;
	virtual void Stop() = 0;
	virtual bool IsRunning() const = 0;
};

std::unique_ptr<VideoControllerTimer> CreateVideoControllerTimer(
	std::function<void()> on_play_timer);
