#include "threaded_ui_timer.h"

#include "main_thread_timer.h"

#include <utility>

namespace {

class ThreadedUiTimer final : public UiTimer {
	MainThreadTimer timer;

public:
	explicit ThreadedUiTimer(std::function<void()> callback)
	: timer(std::move(callback)) {
	}

	void StartOnce(int delay_ms) override {
		timer.StartOnce(delay_ms);
	}

	void StartRepeating(int interval_ms) override {
		timer.StartRepeating(interval_ms);
	}

	void Stop() override {
		timer.Stop();
	}

	bool IsRunning() const override {
		return timer.IsRunning();
	}
};

class ThreadedUiTimerHost final : public UiTimerHost {
public:
	std::unique_ptr<UiTimer> CreateTimer(std::function<void()> callback) override {
		return std::make_unique<ThreadedUiTimer>(std::move(callback));
	}
};

}

std::shared_ptr<UiTimerHost> CreateThreadedUiTimerHost() {
	return std::make_shared<ThreadedUiTimerHost>();
}
