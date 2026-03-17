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
#include <deque>
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

class CompatibilityOverlaySubtitlesProvider final : public SubtitlesProvider {
	void LoadSubtitles(const char *, size_t) override { }

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::CompatibilityFrameOnly;
	}

	void DrawSubtitles(VideoFrame &dst, double) override {
		if (dst.data.size() < 2)
			dst.data.resize(2);
		dst.data[1] = 42;
	}
};

class OverlayVideoProvider final : public VideoProvider {
public:
	void GetFrame(int n, VideoFrame &frame) override {
		frame.width = 2;
		frame.height = 2;
		frame.pitch = 8;
		frame.flipped = false;
		frame.data.assign(16, 0);
		frame.data[0] = static_cast<unsigned char>(n);
	}

	void SetColorSpace(std::string const&) override { }
	int GetFrameCount() const override { return 10000; }
	int GetWidth() const override { return 2; }
	int GetHeight() const override { return 2; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return "BT.709"; }
	std::string GetDecoderName() const override { return "overlay"; }
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

struct OverlayRetentionResult {
	std::string name;
	int requests = 0;
	int held_packets = 0;
	int unique_storages = 0;
	int overflow_hits = 0;
	double overflow_ratio = 0.0;
	double ns_per_packet = 0.0;
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

OverlayRetentionResult run_overlay_retention_scenario(char const *name, int requests, int held_packets) {
	AsyncVideoProvider provider(
		agi::make_unique<OverlayVideoProvider>(),
		agi::make_unique<CompatibilityOverlaySubtitlesProvider>(),
		[](std::unique_ptr<wxEvent>) { });

	auto subs = MakeSubtitleFile();
	provider.LoadSubtitles(&subs);

	std::deque<VideoRenderPacket> held;
	std::vector<void*> unique_storages;
	void* preferred_slot_0 = nullptr;
	void* preferred_slot_1 = nullptr;
	int overflow_hits = 0;

	auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < requests; ++i) {
		auto packet = provider.GetRenderPacket(77, 5000.0);
		void* storage = packet.subtitle_overlay_storage.get();
		if (storage && std::find(unique_storages.begin(), unique_storages.end(), storage) == unique_storages.end())
			unique_storages.push_back(storage);
		if (storage) {
			if (!preferred_slot_0) preferred_slot_0 = storage;
			else if (storage != preferred_slot_0 && !preferred_slot_1) preferred_slot_1 = storage;
			else if (storage != preferred_slot_0 && storage != preferred_slot_1) ++overflow_hits;
		}

		held.push_back(std::move(packet));
		while (static_cast<int>(held.size()) > held_packets)
			held.pop_front();
	}
	auto end = std::chrono::steady_clock::now();

	return {
		name,
		requests,
		held_packets,
		static_cast<int>(unique_storages.size()),
		overflow_hits,
		requests > 0 ? static_cast<double>(overflow_hits) / requests : 0.0,
		std::chrono::duration<double, std::nano>(end - start).count() / requests
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
	auto overlay_results = std::vector<OverlayRetentionResult>{
		run_overlay_retention_scenario("overlay_hold0", 500, 0),
		run_overlay_retention_scenario("overlay_hold1", 500, 1),
		run_overlay_retention_scenario("overlay_hold2", 500, 2),
		run_overlay_retention_scenario("overlay_hold4", 500, 4),
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

	std::cout << "\nCompatibility overlay retention\n";
	std::cout << std::left << std::setw(18) << "scenario"
		<< std::right << std::setw(10) << "requests"
		<< std::setw(10) << "held"
		<< std::setw(12) << "storages"
		<< std::setw(12) << "overflow"
		<< std::setw(14) << "overflow%"
		<< std::setw(16) << "ns/packet"
		<< "\n";
	for (auto const& result : overlay_results) {
		std::cout << std::left << std::setw(18) << result.name
			<< std::right << std::setw(10) << result.requests
			<< std::setw(10) << result.held_packets
			<< std::setw(12) << result.unique_storages
			<< std::setw(12) << result.overflow_hits
			<< std::setw(14) << std::fixed << std::setprecision(2) << (result.overflow_ratio * 100.0)
			<< std::setw(16) << std::fixed << std::setprecision(2) << result.ns_per_packet
			<< "\n";
	}
	return 0;
}
