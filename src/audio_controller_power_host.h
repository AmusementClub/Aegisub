#pragma once

#include <functional>
#include <memory>

class AudioControllerPowerHost {
public:
	virtual ~AudioControllerPowerHost() = default;
};

std::unique_ptr<AudioControllerPowerHost> CreateAudioControllerPowerHost(
	std::function<void()> on_suspend,
	std::function<void()> on_resume);
