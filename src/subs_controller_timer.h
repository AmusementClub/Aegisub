#pragma once

#include <functional>
#include <memory>

class SubsControllerTimer {
public:
	virtual ~SubsControllerTimer() = default;

	virtual void Start(int interval_ms) = 0;
	virtual void Stop() = 0;
	virtual bool IsRunning() const = 0;
};

std::unique_ptr<SubsControllerTimer> CreateSubsControllerTimer(
	std::function<void()> on_timer);
std::unique_ptr<SubsControllerTimer> CreateNullSubsControllerTimer();
