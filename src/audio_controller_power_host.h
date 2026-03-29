#pragma once

#include <functional>
#include <memory>

// Shared audio controller code consumes this abstract power-event contract;
// platform-specific wx binding lives in wx_audio_controller_power_host.cpp.
class AudioControllerPowerHost {
public:
	virtual ~AudioControllerPowerHost() = default;
};

std::unique_ptr<AudioControllerPowerHost> CreateAudioControllerPowerHost(
	std::function<void()> on_suspend,
	std::function<void()> on_resume);
