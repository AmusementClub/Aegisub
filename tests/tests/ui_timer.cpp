#include <main.h>

#include "../../src/audio_controller_timer.h"
#include "../../src/ui_timer.h"
#include "../../src/video_controller_timer.h"

#include <memory>
#include <utility>
#include <vector>

namespace {

class RecordingUiTimer final : public UiTimer {
public:
	int start_once_delay_ms = -1;
	int start_repeat_interval_ms = -1;
	int stop_count = 0;
	bool running = false;

	void StartOnce(int delay_ms) override {
		start_once_delay_ms = delay_ms;
		running = true;
	}

	void StartRepeating(int interval_ms) override {
		start_repeat_interval_ms = interval_ms;
		running = true;
	}

	void Stop() override {
		++stop_count;
		running = false;
	}

	bool IsRunning() const override {
		return running;
	}
};

class RecordingUiTimerHost final : public UiTimerHost {
public:
	std::vector<RecordingUiTimer*> created_timers;

	std::unique_ptr<UiTimer> CreateTimer(std::function<void()>) override {
		auto timer = std::make_unique<RecordingUiTimer>();
		created_timers.push_back(timer.get());
		return timer;
	}
};

class ScopedInstalledUiTimerHost {
	std::shared_ptr<UiTimerHost> previous;

public:
	explicit ScopedInstalledUiTimerHost(std::shared_ptr<UiTimerHost> host)
	: previous(GetUiTimerHost()) {
		InstallUiTimerHost(std::move(host));
	}

	~ScopedInstalledUiTimerHost() {
		InstallUiTimerHost(std::move(previous));
	}
};

}

TEST(ui_timer, install_and_reset_roundtrip_preserves_global_host_slot) {
	ResetUiTimerHost();
	EXPECT_FALSE(GetUiTimerHost());

	auto host = std::make_shared<RecordingUiTimerHost>();
	InstallUiTimerHost(host);
	EXPECT_EQ(host.get(), GetUiTimerHost().get());

	ResetUiTimerHost();
	EXPECT_FALSE(GetUiTimerHost());
}

TEST(video_controller_timer, delegates_start_stop_and_running_state_to_installed_ui_timer_host) {
	auto host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost scope(host);

	auto timer = CreateVideoControllerTimer([] { });
	ASSERT_EQ(1u, host->created_timers.size());

	auto* recorded = host->created_timers.front();
	EXPECT_FALSE(timer->IsRunning());

	timer->Start(42);
	EXPECT_EQ(42, recorded->start_repeat_interval_ms);
	EXPECT_TRUE(timer->IsRunning());

	timer->Stop();
	EXPECT_EQ(1, recorded->stop_count);
	EXPECT_FALSE(timer->IsRunning());
}

TEST(audio_controller_timer, delegates_start_and_stop_to_installed_ui_timer_host) {
	auto host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost scope(host);

	auto timer = CreateAudioControllerTimer([] { });
	ASSERT_EQ(1u, host->created_timers.size());

	auto* recorded = host->created_timers.front();
	timer->Start(20);
	EXPECT_EQ(20, recorded->start_repeat_interval_ms);
	EXPECT_TRUE(recorded->running);

	timer->Stop();
	EXPECT_EQ(1, recorded->stop_count);
	EXPECT_FALSE(recorded->running);
}
