#pragma once

#include <functional>
#include <memory>

class UiTimer {
public:
	virtual ~UiTimer() = default;

	virtual void StartOnce(int delay_ms) = 0;
	virtual void StartRepeating(int interval_ms) = 0;
	virtual void Stop() = 0;
	virtual bool IsRunning() const = 0;
};

class UiTimerHost {
public:
	virtual ~UiTimerHost() = default;

	virtual std::unique_ptr<UiTimer> CreateTimer(std::function<void()> callback) = 0;
};

void InstallUiTimerHost(std::shared_ptr<UiTimerHost> host);
std::shared_ptr<UiTimerHost> GetUiTimerHost();
void ResetUiTimerHost();
