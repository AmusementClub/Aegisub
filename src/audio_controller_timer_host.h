#pragma once

#include <functional>
#include <memory>

class AudioControllerTimerHost {
public:
	virtual ~AudioControllerTimerHost() = default;

	virtual void Start(int interval_ms) = 0;
	virtual void Stop() = 0;
};

std::unique_ptr<AudioControllerTimerHost> CreateAudioControllerTimerHost(
	std::function<void()> on_playback_timer);
