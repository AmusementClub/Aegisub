#include "options.h"
#include "include/aegisub/video_provider.h"
#include "video_frame.h"
#include "video_provider_manager.h"

#include <libaegisub/vfr.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace Automation4 { class AutoloadScriptManager; }

namespace config {
	agi::Options *opt = nullptr;
	agi::MRUManager *mru = nullptr;
	agi::Path *path = nullptr;
	Automation4::AutoloadScriptManager *global_scripts = nullptr;
}

namespace {
using clock_type = std::chrono::steady_clock;

struct CountingVideoProvider final : VideoProvider {
	std::size_t frame_bytes;
	int get_frame_calls = 0;

	explicit CountingVideoProvider(std::size_t frame_bytes)
	: frame_bytes(frame_bytes) {
	}

	void GetFrame(int n, VideoFrame &frame) override {
		++get_frame_calls;
		frame.width = 64;
		frame.height = 64;
		frame.pitch = 64 * 4;
		frame.flipped = false;
		frame.data.assign(frame_bytes, static_cast<unsigned char>(n));
	}

	void SetColorSpace(std::string const&) override { }
	int GetFrameCount() const override { return 10000; }
	int GetWidth() const override { return 64; }
	int GetHeight() const override { return 64; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return "BT.709"; }
	std::string GetDecoderName() const override { return "counting"; }
};

struct BenchRow {
	std::string name;
	double ns_per_request = 0.0;
	double hit_rate = 0.0;
	int provider_calls = 0;
	std::size_t requests = 0;
};

template<typename Workload>
BenchRow run_workload(std::string name, std::size_t cache_frames, Workload&& workload) {
	auto raw = new CountingVideoProvider(64 * 64 * 4);
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64 * 64 * 4 * cache_frames);
	VideoFrame frame;

	auto requests = workload();
	auto start = clock_type::now();
	for (int request : requests)
		cache->GetFrame(request, frame);
	auto end = clock_type::now();

	double ns_per_request = std::chrono::duration<double, std::nano>(end - start).count() / requests.size();
	double hit_rate = 1.0 - static_cast<double>(raw->get_frame_calls) / requests.size();
	return {std::move(name), ns_per_request, hit_rate, raw->get_frame_calls, requests.size()};
}

std::vector<int> make_sequential() {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 4000; ++i)
		requests.push_back(i);
	return requests;
}

std::vector<int> make_reverse_linear() {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 3999; i >= 0; --i)
		requests.push_back(i);
	return requests;
}

std::vector<int> make_small_window() {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 4000; ++i)
		requests.push_back(200 + (i % 12));
	return requests;
}

std::vector<int> make_local_jitter() {
	std::vector<int> requests;
	requests.reserve(4000);
	int base = 1000;
	for (int i = 0; i < 1000; ++i) {
		requests.push_back(base);
		requests.push_back(base + 1);
		requests.push_back(base - 1);
		requests.push_back(base);
		base += (i % 7 == 0) ? 1 : 0;
	}
	return requests;
}

std::vector<int> make_hot_spots() {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 4000; ++i)
		requests.push_back((i % 5) * 3);
	return requests;
}

std::vector<int> make_frame_step_ping_pong() {
	std::vector<int> requests;
	requests.reserve(4000);
	int frame = 1000;
	for (int i = 0; i < 2000; ++i) {
		requests.push_back(frame++);
		requests.push_back(frame - 2);
	}
	return requests;
}

std::vector<int> make_working_set_fit(std::size_t cache_frames) {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 4000; ++i)
		requests.push_back(500 + (i % static_cast<int>(cache_frames)));
	return requests;
}

std::vector<int> make_working_set_plus_one(std::size_t cache_frames) {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 4000; ++i)
		requests.push_back(500 + (i % static_cast<int>(cache_frames + 1)));
	return requests;
}

std::vector<int> make_nearby_seek_bursts() {
	std::vector<int> requests;
	requests.reserve(4000);
	int base = 500;
	for (int i = 0; i < 250; ++i) {
		for (int j = 0; j < 8; ++j)
			requests.push_back(base + (j % 6));
		base += 3;
	}
	return requests;
}

std::vector<int> make_far_seek_bursts() {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 500; ++i) {
		int base = (i * 197) % 8000;
		for (int j = 0; j < 8; ++j)
			requests.push_back(base + j);
	}
	return requests;
}

std::vector<int> make_same_frame_requery() {
	std::vector<int> requests;
	requests.reserve(4000);
	for (int i = 0; i < 4000; ++i)
		requests.push_back(777);
	return requests;
}
}

int main() {
	std::vector<BenchRow> results;
	for (auto cache_frames : {std::size_t(2), std::size_t(8), std::size_t(32)}) {
		auto prefix = std::to_string(cache_frames) + "f/";
		results.push_back(run_workload(prefix + "playback_linear", cache_frames, make_sequential));
		results.push_back(run_workload(prefix + "reverse_linear", cache_frames, make_reverse_linear));
		results.push_back(run_workload(prefix + "seek_small_window", cache_frames, make_small_window));
		results.push_back(run_workload(prefix + "local_jitter", cache_frames, make_local_jitter));
		results.push_back(run_workload(prefix + "frame_step_pingpong", cache_frames, make_frame_step_ping_pong));
		results.push_back(run_workload(prefix + "working_set_fit", cache_frames, [=] { return make_working_set_fit(cache_frames); }));
		results.push_back(run_workload(prefix + "working_set_plus_one", cache_frames, [=] { return make_working_set_plus_one(cache_frames); }));
		results.push_back(run_workload(prefix + "seek_nearby_bursts", cache_frames, make_nearby_seek_bursts));
		results.push_back(run_workload(prefix + "seek_far_bursts", cache_frames, make_far_seek_bursts));
		results.push_back(run_workload(prefix + "same_frame_requery", cache_frames, make_same_frame_requery));
		results.push_back(run_workload(prefix + "hot_spots", cache_frames, make_hot_spots));
	}

	std::cout << "Video cache benchmark\n";
	std::cout << std::left << std::setw(22) << "workload"
		<< std::right << std::setw(14) << "ns/request"
		<< std::setw(12) << "hit_rate"
		<< std::setw(16) << "provider_calls"
		<< std::setw(12) << "requests"
		<< "\n";

	for (auto const& result : results) {
		std::cout << std::left << std::setw(22) << result.name
			<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << result.ns_per_request
			<< std::setw(12) << std::fixed << std::setprecision(3) << result.hit_rate
			<< std::setw(16) << result.provider_calls
			<< std::setw(12) << result.requests
			<< "\n";
	}

	return 0;
}
