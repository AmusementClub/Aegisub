#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "export_fixstyle.h"
#include "include/aegisub/subtitles_provider.h"
#include "include/aegisub/video_provider.h"
#include "options.h"
#include "video_frame.h"
#include "video_provider_manager.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/vfr.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Automation4 { class AutoloadScriptManager; }

namespace config {
	agi::Options *opt = nullptr;
	agi::MRUManager *mru = nullptr;
	agi::Path *path = nullptr;
	Automation4::AutoloadScriptManager *global_scripts = nullptr;
}

namespace {
struct CountingState {
	std::mutex mutex;
	int render_calls = 0;
	int delivered_frame = -1;
	int latest_requested_frame = -1;
	int delivered_events = 0;
	bool saw_stale_delivery = false;
	std::chrono::steady_clock::time_point latest_request_time;
	double latest_latency_ms = -1.0;
	std::condition_variable cv;
};

class SlowVideoProvider final : public VideoProvider {
	std::shared_ptr<CountingState> state;
	int decode_ms;

public:
	SlowVideoProvider(std::shared_ptr<CountingState> state, int decode_ms)
	: state(std::move(state))
	, decode_ms(decode_ms) {
	}

	void GetFrame(int n, VideoFrame &frame) override {
		std::this_thread::sleep_for(std::chrono::milliseconds(decode_ms));
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			++state->render_calls;
		}
		frame.width = 2;
		frame.height = 2;
		frame.pitch = 8;
		frame.flipped = false;
		frame.data = {static_cast<unsigned char>(n), 0, 0, 0, 0, 0, 0, 0};
	}

	void SetColorSpace(std::string const&) override { }
	int GetFrameCount() const override { return 10000; }
	int GetWidth() const override { return 2; }
	int GetHeight() const override { return 2; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return "BT.709"; }
	std::string GetDecoderName() const override { return "slow"; }
};

class NoopSubtitlesProvider final : public SubtitlesProvider {
	void LoadSubtitles(const char *, size_t) override { }
	void DrawSubtitles(VideoFrame &, double) override { }
};

struct ScenarioResult {
	std::string name;
	int requests = 0;
	int render_calls = 0;
	int delivered_events = 0;
	int dropped_requests = 0;
	bool latest_delivered = false;
	bool saw_stale_delivery = false;
	double latest_latency_ms = -1.0;
};

AssFile MakeSubtitleFile() {
	AssFile file;
	auto *line = new AssDialogue;
	line->Start = 0;
	line->End = 10000;
	line->Row = 0;
	line->Text = "bench";
	file.Events.push_back(*line);
	return file;
}

using FramePattern = std::function<int(int)>;
}

std::vector<std::string> VideoProviderFactory::GetClasses() { return {}; }
std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() { return {}; }
std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *) { return nullptr; }
std::vector<std::string> SubtitlesProviderFactory::GetClasses() { return {}; }
std::unique_ptr<SubtitlesProvider> SubtitlesProviderFactory::GetProvider(agi::BackgroundRunner *) { return nullptr; }
void SubtitlesProvider::LoadSubtitles(AssFile *, int) {
	static const char payload[] = "bench";
	LoadSubtitles(payload, sizeof(payload) - 1);
}
void AssFixStylesFilter::ProcessSubs(AssFile *) { }

ScenarioResult run_scenario(char const *name, int requests, int decode_ms, int request_gap_ms, FramePattern pattern, int subtitle_update_interval = 0) {
	auto state = std::make_shared<CountingState>();
	AsyncVideoProvider provider(
		agi::make_unique<SlowVideoProvider>(state, decode_ms),
		agi::make_unique<NoopSubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) {
			if (evt->GetEventType() != EVT_FRAME_READY)
				return;
			auto *frame_evt = static_cast<FrameReadyEvent *>(evt.get());
			auto delivered_frame = static_cast<int>(frame_evt->time / 1000.0);
			auto now = std::chrono::steady_clock::now();
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				++state->delivered_events;
				state->saw_stale_delivery = state->saw_stale_delivery || delivered_frame != state->latest_requested_frame;
				state->delivered_frame = delivered_frame;
				if (delivered_frame == state->latest_requested_frame) {
					state->latest_latency_ms = std::chrono::duration<double, std::milli>(now - state->latest_request_time).count();
				}
			}
			state->cv.notify_all();
		});

	auto subs = MakeSubtitleFile();
	provider.LoadSubtitles(&subs);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->delivered_frame = -1;
		state->latest_requested_frame = -1;
	}

	for (int i = 0; i < requests; ++i) {
		auto frame_number = pattern(i);
		if (subtitle_update_interval > 0 && i % subtitle_update_interval == 0) {
			subs.Events.front().Text = i % 2 == 0 ? "bench-a" : "bench-b";
			provider.LoadSubtitles(&subs);
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->latest_requested_frame = frame_number;
			state->latest_request_time = std::chrono::steady_clock::now();
		}
		provider.RequestFrame(frame_number, frame_number * 1000.0);
		if (request_gap_ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(request_gap_ms));
	}

	std::unique_lock<std::mutex> lock(state->mutex);
	auto expected_frame = pattern(requests - 1);
	state->cv.wait_for(lock, std::chrono::seconds(5), [&] { return state->delivered_frame == expected_frame; });

	return {
		name,
		requests,
		state->render_calls,
		state->delivered_events,
		requests - state->render_calls,
		state->delivered_frame == expected_frame,
		state->saw_stale_delivery,
		state->latest_latency_ms
	};
}

int main() {
	agi::dispatch::Init([](agi::dispatch::Thunk thunk) { thunk(); });

	auto results = std::vector<ScenarioResult>{
		run_scenario("burst_seek_1ms", 1000, 1, 0, [](int i) { return i; }),
		run_scenario("burst_seek_5ms", 1000, 5, 0, [](int i) { return i; }),
		run_scenario("paced_seek_5ms_gap1", 300, 5, 1, [](int i) { return i; }),
		run_scenario("paced_seek_5ms_gap6", 200, 5, 6, [](int i) { return i; }),
		run_scenario("paced_seek_5ms_gap12", 120, 5, 12, [](int i) { return i; }),
		run_scenario("small_window_3ms", 800, 3, 0, [](int i) { return 200 + (i % 12); }),
		run_scenario("same_frame_3ms", 500, 3, 0, [](int) { return 777; }),
		run_scenario("seek_subs_5ms", 600, 5, 0, [](int i) { return i; }, 20),
	};

	std::cout << "Async video provider stress\n";
	std::cout << std::left << std::setw(18) << "scenario"
		<< std::right << std::setw(10) << "requests"
		<< std::setw(10) << "renders"
		<< std::setw(10) << "events"
		<< std::setw(10) << "dropped"
		<< std::setw(12) << "latest"
		<< std::setw(12) << "stale"
		<< std::setw(16) << "latency_ms"
		<< "\n";
	for (auto const& result : results) {
		std::cout << std::left << std::setw(18) << result.name
			<< std::right << std::setw(10) << result.requests
			<< std::setw(10) << result.render_calls
			<< std::setw(10) << result.delivered_events
			<< std::setw(10) << result.dropped_requests
			<< std::setw(12) << (result.latest_delivered ? "true" : "false")
			<< std::setw(12) << (result.saw_stale_delivery ? "true" : "false")
			<< std::setw(16) << std::fixed << std::setprecision(2) << result.latest_latency_ms
			<< "\n";
	}
	return 0;
}
