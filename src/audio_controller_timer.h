#pragma once

#include <functional>
#include <memory>

class AudioControllerTimer {
public:
	virtual ~AudioControllerTimer() = default;

	virtual void Start(int interval_ms) = 0;
	virtual void Stop() = 0;
};

std::unique_ptr<AudioControllerTimer> CreateAudioControllerTimer(
	std::function<void()> on_playback_timer);
