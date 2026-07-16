#include "../../src/skia/audio/skia_audio_content_worker.h"

#include <libaegisub/audio/provider.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace aegisub::skia::audio;
using namespace std::chrono_literals;

class GateAudioProvider final : public agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable bool entered = false;
	mutable bool released = true;
	mutable std::atomic<std::uint64_t> fills { 0 };

protected:
	void FillBuffer(void *buffer, std::int64_t start, std::int64_t count) const override {
		++fills;
		{
			std::unique_lock<std::mutex> lock(mutex);
			entered = true;
			condition.notify_all();
			condition.wait(lock, [&] { return released; });
		}

		auto *samples = static_cast<float *>(buffer);
		for (std::int64_t frame = 0; frame < count; ++frame) {
			auto const value = static_cast<float>(((start + frame) % 101) - 50) / 50.f;
			for (int channel = 0; channel < channels; ++channel)
				samples[static_cast<std::size_t>(frame) * channels + channel] = value;
		}
	}

public:
	explicit GateAudioProvider(bool initially_blocked = false) {
		channels = 2;
		num_samples = 48000 * 60;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(float);
		float_samples = true;
		released = !initially_blocked;
	}

	bool WaitUntilEntered(std::chrono::milliseconds timeout = 2s) const {
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [&] { return entered; });
	}

	void Release() const {
		{
			std::lock_guard<std::mutex> lock(mutex);
			released = true;
		}
		condition.notify_all();
	}

	std::uint64_t FillCount() const { return fills.load(); }
};

ContentAnalysisConfig WaveformAnalysis() {
	ContentAnalysisConfig config;
	config.kind = ContentKind::Waveform;
	config.source_mode = ContentSourceMode::FloatInterleaved;
	config.milliseconds_per_pixel = 1.0;
	config.mix_policy = AudioMixPolicy::MonoAverage;
	return config;
}

ContentViewportRequest WaveformViewport(
	ContentGeneration generation,
	std::uint64_t first_column,
	std::uint32_t column_count) {
	ContentViewportRequest request;
	request.generation = generation;
	request.kind = ContentKind::Waveform;
	request.first_column = first_column;
	request.column_count = column_count;
	request.tile_column_count = 64;
	return request;
}

ContentTileKey FirstKey(ContentViewportRequest const& request) {
	auto keys = PlanVisibleContentTiles(request);
	EXPECT_FALSE(keys.empty());
	return keys.front();
}

class ReadyLatch {
	std::mutex mutex;
	std::condition_variable condition;
	std::uint64_t count = 0;

public:
	void Notify(ContentGeneration) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			++count;
		}
		condition.notify_all();
	}

	bool WaitFor(std::uint64_t expected, std::chrono::milliseconds timeout = 2s) {
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [&] { return count >= expected; });
	}
};

bool WaitForBuilds(
	ContentWorker const& worker,
	std::uint64_t expected,
	std::chrono::milliseconds timeout = 2s) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		auto const metrics = worker.Metrics();
		if (metrics.builds_ready >= expected && !metrics.build_active && !metrics.request_pending)
			return true;
		std::this_thread::sleep_for(1ms);
	}
	return false;
}

}

TEST(skia_audio_content_worker, request_builds_only_on_worker_and_notifies_ready) {
	GateAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	auto const viewport = WaveformViewport(generation, 0, 64);
	auto const key = FirstKey(viewport);

	EXPECT_EQ(provider.FillCount(), 0u);
	worker.Request(viewport);
	ASSERT_TRUE(ready.WaitFor(1));

	auto tile = worker.Find(key);
	ASSERT_NE(tile, nullptr);
	EXPECT_EQ(tile->key, key);
	EXPECT_TRUE(tile->IsValid());
	EXPECT_GT(provider.FillCount(), 0u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, latest_viewport_takes_over_after_inflight_tile_completes) {
	GateAudioProvider provider(true);
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	auto const obsolete = WaveformViewport(generation, 0, 256);
	auto const latest = WaveformViewport(generation, 1024, 64);
	auto const obsolete_key = FirstKey(obsolete);
	auto const latest_key = FirstKey(latest);

	worker.Request(obsolete);
	ASSERT_TRUE(provider.WaitUntilEntered());
	worker.Request(latest);
	provider.Release();
	ASSERT_TRUE(ready.WaitFor(2));

	EXPECT_NE(worker.Find(obsolete_key), nullptr);
	EXPECT_NE(worker.Find(latest_key), nullptr);
	auto const metrics = worker.Metrics();
	EXPECT_GE(metrics.superseded_requests, 1u);
	EXPECT_EQ(metrics.builds_cancelled, 0u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, request_builds_adjacent_prefetch_without_extra_notifications) {
	GateAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	auto viewport = WaveformViewport(generation, 64, 64);
	viewport.prefetch_tile_count = 1;
	worker.Request(viewport);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 3));

	auto visible = FirstKey(viewport);
	auto before = visible;
	--before.tile_index;
	auto after = visible;
	++after.tile_index;
	EXPECT_NE(worker.Find(visible), nullptr);
	EXPECT_NE(worker.Find(before), nullptr);
	EXPECT_NE(worker.Find(after), nullptr);
	EXPECT_EQ(worker.Metrics().ready_notifications, 1u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, prefetch_never_evicts_visible_tile_from_tight_budget) {
	GateAudioProvider provider;
	ReadyLatch ready;
	auto const one_tile_budget = sizeof(ContentTile) + 64 * sizeof(WaveformColumn);
	ContentWorker worker(
		[&](ContentGeneration generation) { ready.Notify(generation); },
		{},
		one_tile_budget);

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	auto viewport = WaveformViewport(generation, 64, 64);
	viewport.prefetch_tile_count = 1;
	worker.Request(viewport);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 1));

	auto visible = FirstKey(viewport);
	auto before = visible;
	--before.tile_index;
	auto after = visible;
	++after.tile_index;
	EXPECT_NE(worker.Find(visible), nullptr);
	EXPECT_EQ(worker.Find(before), nullptr);
	EXPECT_EQ(worker.Find(after), nullptr);
	EXPECT_EQ(worker.StoreMetrics().evictions, 0u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, provider_detach_waits_for_inflight_read_before_returning) {
	GateAudioProvider provider(true);
	ContentWorker worker;

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	worker.Request(WaveformViewport(generation, 0, 64));
	ASSERT_TRUE(provider.WaitUntilEntered());

	auto detached = std::async(std::launch::async, [&] { return worker.SetProvider(nullptr); });
	EXPECT_EQ(detached.wait_for(20ms), std::future_status::timeout);
	provider.Release();
	EXPECT_EQ(detached.wait_for(2s), std::future_status::ready);
	auto const detached_generation = detached.get();
	EXPECT_NE(detached_generation.provider, generation.provider);
	EXPECT_FALSE(worker.Metrics().provider_attached);
}

TEST(skia_audio_content_worker, analysis_change_still_cancels_inflight_generation) {
	GateAudioProvider provider(true);
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const old_generation = worker.SetAnalysis(WaveformAnalysis());
	auto const old_viewport = WaveformViewport(old_generation, 0, 64);
	worker.Request(old_viewport);
	ASSERT_TRUE(provider.WaitUntilEntered());

	auto changed = WaveformAnalysis();
	changed.milliseconds_per_pixel = 2.0;
	auto const new_generation = worker.SetAnalysis(changed);
	auto const new_viewport = WaveformViewport(new_generation, 0, 64);
	worker.Request(new_viewport);
	provider.Release();
	ASSERT_TRUE(ready.WaitFor(1));

	EXPECT_EQ(worker.Find(FirstKey(old_viewport)), nullptr);
	EXPECT_NE(worker.Find(FirstKey(new_viewport)), nullptr);
	EXPECT_GE(worker.Metrics().builds_cancelled, 1u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, analysis_change_drops_old_generation_and_rejects_old_request) {
	GateAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const old_generation = worker.SetAnalysis(WaveformAnalysis());
	auto old_viewport = WaveformViewport(old_generation, 0, 64);
	worker.Request(old_viewport);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_NE(worker.Find(FirstKey(old_viewport)), nullptr);

	auto changed = WaveformAnalysis();
	changed.milliseconds_per_pixel = 2.0;
	auto const new_generation = worker.SetAnalysis(changed);
	EXPECT_NE(new_generation.analysis, old_generation.analysis);
	EXPECT_EQ(worker.Find(FirstKey(old_viewport)), nullptr);

	worker.Request(old_viewport);
	EXPECT_EQ(worker.Metrics().requests, 1u);
	worker.SetProvider(nullptr);
}
