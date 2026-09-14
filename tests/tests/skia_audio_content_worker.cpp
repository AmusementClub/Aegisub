#include "../../src/skia/audio/skia_audio_content_worker.h"
#include "../../src/perf_trace.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace aegisub::skia::audio;
using namespace std::chrono_literals;

std::string ReadAll(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::in | std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

class GateAudioProvider final : public agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable bool entered = false;
	mutable bool released = true;
	mutable std::atomic<std::uint64_t> fills { 0 };
	mutable std::atomic<int> failed_fills_remaining{0};

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
		if (failed_fills_remaining > 0) {
			--failed_fills_remaining;
			std::fill_n(samples, static_cast<std::size_t>(count) * channels / 2, 0.75f);
			throw agi::AudioDecodeError("injected partial audio read");
		}
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

	void BlockNextFill() const {
		std::lock_guard<std::mutex> lock(mutex);
		entered = false;
		released = false;
	}

	std::uint64_t FillCount() const { return fills.load(); }
	void FailNextFill(int count = 1) { failed_fills_remaining = count; }
};

class IncrementalAudioProvider final : public agi::AudioProvider {
	mutable std::atomic<std::uint64_t> fills { 0 };

protected:
	void FillBuffer(void *buffer, std::int64_t start, std::int64_t count) const override {
		++fills;
		auto const frontier = GetDecodedSamples();
		auto *samples = static_cast<float *>(buffer);
		for (std::int64_t frame = 0; frame < count; ++frame) {
			auto const sample = start + frame;
			samples[frame] = sample < frontier
				? static_cast<float>((sample % 101) - 50) / 50.f
				: 0.f;
		}
	}

public:
	IncrementalAudioProvider() {
		channels = 1;
		num_samples = 48000 * 60;
		decoded_samples = 0;
		sample_rate = 48000;
		bytes_per_sample = sizeof(float);
		float_samples = true;
	}

	void AdvanceDecodedTo(std::int64_t samples) {
		decoded_samples = std::clamp<std::int64_t>(samples, 0, num_samples);
	}

	std::uint64_t FillCount() const { return fills.load(); }
};

class ObsoleteReadProvider final : public agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable bool entered = false;
	mutable bool released = false;
	mutable std::atomic<int> failed_reads{0};
	mutable std::atomic<bool> gate_timed_out{false};

	protected:
	void FillBuffer(void *buffer, std::int64_t start, std::int64_t count) const override {
		if (start == 0) {
			std::unique_lock lock(mutex);
			entered = true;
			condition.notify_all();
			if (!condition.wait_for(lock, 2s, [&] { return released; })) {
				gate_timed_out = true;
				throw agi::AudioDecodeError("obsolete-read gate timed out");
			}
			++failed_reads;
			throw agi::AudioDecodeError("obsolete request read failed");
		}
		auto *samples = static_cast<float *>(buffer);
		for (std::int64_t frame = 0; frame < count; ++frame)
			samples[frame] = (start + frame) % 2 == 0 ? 0.5f : -0.25f;
	}

	public:
	ObsoleteReadProvider() {
		channels = 1;
		num_samples = 48000 * 60;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(float);
		float_samples = true;
	}

	bool WaitUntilEntered() const {
		std::unique_lock lock(mutex);
		return condition.wait_for(lock, 2s, [&] { return entered; });
	}

	void Release() const {
		{
			std::scoped_lock lock(mutex);
			released = true;
		}
		condition.notify_all();
	}

	int FailedReads() const { return failed_reads.load(); }
	bool GateTimedOut() const { return gate_timed_out.load(); }
};

ContentAnalysisConfig WaveformAnalysis() {
	ContentAnalysisConfig config;
	config.kind = ContentKind::Waveform;
	config.source_mode = ContentSourceMode::FloatInterleaved;
	config.milliseconds_per_pixel = 1.0;
	config.mix_policy = AudioMixPolicy::MonoAverage;
	return config;
}

ContentAnalysisConfig SpectrumAnalysis() {
	ContentAnalysisConfig config;
	config.kind = ContentKind::Spectrum;
	config.source_mode = ContentSourceMode::FloatInterleaved;
	config.milliseconds_per_pixel = 1.0;
	config.mix_policy = AudioMixPolicy::MonoAverage;
	config.spectrum_channel_mode = SpectrumChannelMode::MixedMono;
	config.spectrum_derivation_size = 4;
	config.spectrum_derivation_distance = 4;
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

ContentViewportRequest SpectrumViewport(
	ContentGeneration generation,
	std::uint64_t first_column,
	std::uint32_t column_count) {
	ContentViewportRequest request;
	request.generation = generation;
	request.kind = ContentKind::Spectrum;
	request.first_column = first_column;
	request.column_count = column_count;
	request.tile_column_count = 4;
	request.spectrum_bin_count = 16;
	return request;
}

std::shared_ptr<SpectrumBandPlan const> SpectrumPlan(
	SpectrumScaleMode mode = SpectrumScaleMode::LegacyLinear,
	int output_height = 8) {
	SpectrumBandPlanRequest request;
	request.bin_count = 16;
	request.output_height = output_height;
	request.sample_rate = 48000;
	request.mode = mode;
	request.frequency_reference_position = mode == SpectrumScaleMode::LegacyLinear
		? 1.f / 3.f : 0.425f;
	auto plan = std::make_shared<SpectrumBandPlan>(BuildSpectrumBandPlan(request));
	EXPECT_TRUE(plan->IsValid());
	return plan;
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

bool WaitForPayloadBuilds(
	ContentWorker const& worker,
	std::uint64_t expected,
	std::chrono::milliseconds timeout = 2s) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		auto const metrics = worker.Metrics();
		if (metrics.payload_builds_ready >= expected
			&& !metrics.build_active
			&& !metrics.request_pending) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return false;
}

bool WaitForAnalysisBudget(
	ContentWorker const& worker,
	std::size_t expected,
	std::chrono::milliseconds timeout = 2s) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		auto const metrics = worker.AnalysisMetrics();
		if (metrics.configured_spectrum_budget_bytes == expected
			&& metrics.spectrum_cache_count > 0
			&& metrics.spectrum_cache_budget_bytes <= expected) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return false;
}

bool WaitForDecodeDeferrals(
	ContentWorker const& worker,
	std::uint64_t expected,
	std::chrono::milliseconds timeout = 2s) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		auto const metrics = worker.Metrics();
		if (metrics.decode_deferred_tiles >= expected
			&& !metrics.build_active
			&& !metrics.request_pending) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return false;
}

void ExpectVisiblePayloadCacheHitNotifiesAfterPrefetchRace(ContentKind kind) {
	GateAudioProvider provider;
	ReadyLatch ready;
	std::mutex callback_mutex;
	std::condition_variable callback_condition;
	bool first_callback_entered = false;
	bool release_first_callback = false;
	ContentWorker worker([&](ContentGeneration generation) {
		ready.Notify(generation);
		std::unique_lock<std::mutex> lock(callback_mutex);
		if (first_callback_entered)
			return;
		first_callback_entered = true;
		callback_condition.notify_all();
		callback_condition.wait(lock, [&] { return release_first_callback; });
	});

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(
		kind == ContentKind::Waveform ? WaveformAnalysis() : SpectrumAnalysis());
	auto old_viewport = kind == ContentKind::Waveform
		? WaveformViewport(generation, 64, 64)
		: SpectrumViewport(generation, 4, 4);
	old_viewport.prefetch_tile_count = 1;
	auto prefetched_key = FirstKey(old_viewport);
	++prefetched_key.tile_index;
	auto latest_viewport = kind == ContentKind::Waveform
		? WaveformViewport(generation, 128, 64)
		: SpectrumViewport(generation, 8, 4);
	auto const band_plan = kind == ContentKind::Spectrum ? SpectrumPlan() : nullptr;

	worker.Request(old_viewport, band_plan);
	bool callback_entered = false;
	{
		std::unique_lock<std::mutex> lock(callback_mutex);
		callback_entered = callback_condition.wait_for(
			lock, 2s, [&] { return first_callback_entered; });
	}
	if (!callback_entered) {
		{
			std::lock_guard<std::mutex> lock(callback_mutex);
			release_first_callback = true;
		}
		callback_condition.notify_all();
		worker.SetProvider(nullptr);
		FAIL() << "visible tile did not reach the ready callback";
	}

	// Let the old request enter its adjacent prefetch read, then replace it
	// with a viewport for that same tile while the read is in flight.
	provider.BlockNextFill();
	{
		std::lock_guard<std::mutex> lock(callback_mutex);
		release_first_callback = true;
	}
	callback_condition.notify_all();
	if (!provider.WaitUntilEntered()) {
		provider.Release();
		worker.SetProvider(nullptr);
		FAIL() << "prefetch tile did not enter the provider";
	}
	worker.Request(latest_viewport, band_plan);
	provider.Release();

	ASSERT_TRUE(ready.WaitFor(2));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 2));
	EXPECT_NE(worker.FindPayload(MakeContentUploadPayloadKey(
		prefetched_key, band_plan.get())), nullptr);
	EXPECT_EQ(worker.Metrics().ready_notifications, 2u);
	worker.SetProvider(nullptr);
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
	auto payload = worker.FindPayload(MakeContentUploadPayloadKey(key));
	ASSERT_NE(payload, nullptr);
	EXPECT_TRUE(payload->IsValid());
	EXPECT_EQ(1u, worker.Metrics().payload_builds_ready);
	EXPECT_GT(provider.FillCount(), 0u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, waveform_waits_for_complete_decoded_tile_before_publishing) {
	IncrementalAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	auto const viewport = WaveformViewport(generation, 0, 64);
	auto const key = FirstKey(viewport);
	worker.Request(viewport);
	ASSERT_TRUE(WaitForDecodeDeferrals(worker, 1));
	EXPECT_EQ(0u, provider.FillCount());
	EXPECT_EQ(nullptr, worker.Find(key));
	EXPECT_EQ(0u, worker.Metrics().builds_started);

	provider.AdvanceDecodedTo(4096);
	worker.Request(viewport);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 1));
	auto const tile = worker.Find(key);
	ASSERT_NE(nullptr, tile);
	ASSERT_FALSE(tile->waveform.empty());
	EXPECT_TRUE(std::any_of(tile->waveform.begin(), tile->waveform.end(), [](WaveformColumn const& column) {
		return column.peak_min < 0.f || column.peak_max > 0.f;
	}));
	EXPECT_NE(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(key)));
	EXPECT_GT(provider.FillCount(), 0u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, spectrum_waits_for_complete_fft_windows_before_publishing) {
	IncrementalAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(SpectrumAnalysis());
	auto const viewport = SpectrumViewport(generation, 0, 4);
	auto const key = FirstKey(viewport);
	auto const band_plan = SpectrumPlan();
	worker.Request(viewport, band_plan);
	ASSERT_TRUE(WaitForDecodeDeferrals(worker, 1));
	EXPECT_EQ(0u, provider.FillCount());
	EXPECT_EQ(nullptr, worker.Find(key));
	EXPECT_EQ(0u, worker.Metrics().builds_started);
	EXPECT_EQ(0u, worker.AnalysisMetrics().spectrum_cache_misses);

	provider.AdvanceDecodedTo(4096);
	worker.Request(viewport, band_plan);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 1));
	auto const tile = worker.Find(key);
	ASSERT_NE(nullptr, tile);
	ASSERT_FALSE(tile->spectrum_power.empty());
	EXPECT_TRUE(std::any_of(tile->spectrum_power.begin(), tile->spectrum_power.end(), [](float power) {
		return power > 0.f;
	}));
	EXPECT_NE(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(key, band_plan.get())));
	EXPECT_GT(provider.FillCount(), 0u);
	EXPECT_GT(worker.AnalysisMetrics().spectrum_cache_misses, 0u);
	worker.SetProvider(nullptr);
}

namespace {
void ExpectSpectrumScrollRecoversPartialRead(bool memory_cache) {
	auto source = std::make_unique<GateAudioProvider>();
	auto *raw = source.get();
	std::unique_ptr<agi::AudioProvider> provider = std::move(source);
	std::unique_ptr<agi::AudioProvider> reference_provider = std::make_unique<GateAudioProvider>();
	if (memory_cache) {
		raw->FailNextFill();
		provider = agi::CreateRAMAudioProvider(agi::CreateConvertAudioProvider(std::move(provider)));
		reference_provider = agi::CreateRAMAudioProvider(agi::CreateConvertAudioProvider(std::move(reference_provider)));
		auto const deadline = std::chrono::steady_clock::now() + 2s;
		while ((provider->GetDecodedSamples() != provider->GetNumSamples() || reference_provider->GetDecodedSamples() != reference_provider->GetNumSamples()) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(1ms);
		ASSERT_EQ(provider->GetNumSamples(), provider->GetDecodedSamples());
		ASSERT_EQ(reference_provider->GetNumSamples(), reference_provider->GetDecodedSamples());
	}
	ReadyLatch ready;
	ReadyLatch reference_ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });
	ContentWorker reference_worker([&](ContentGeneration generation) { reference_ready.Notify(generation); });
	worker.SetProvider(provider.get());
	reference_worker.SetProvider(reference_provider.get());
	auto analysis = SpectrumAnalysis();
	analysis.milliseconds_per_pixel = 0.25;
	if (memory_cache)
		analysis.source_mode = ContentSourceMode::Int16Mono;
	auto const generation = worker.SetAnalysis(analysis);
	ASSERT_EQ(generation, reference_worker.SetAnalysis(analysis));
	auto const plan = SpectrumPlan();
	// The RAM case starts in a healthy block, then scrolls to the failed block.
	auto viewport = SpectrumViewport(generation, memory_cache ? 180000 : 0, 4);
	worker.Request(viewport, plan);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 1));

	if (!memory_cache)
		raw->FailNextFill();
	viewport.first_column = 128;
	worker.Request(viewport, plan);
	reference_worker.Request(viewport, plan);
	ASSERT_TRUE(ready.WaitFor(2));
	ASSERT_TRUE(reference_ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 2));
	ASSERT_TRUE(WaitForPayloadBuilds(reference_worker, 1));
	auto const key = FirstKey(viewport);
	auto const actual = worker.Find(key);
	auto const expected = reference_worker.Find(key);
	ASSERT_NE(nullptr, actual);
	ASSERT_NE(nullptr, expected);
	EXPECT_EQ(expected->spectrum_power, actual->spectrum_power);
	auto const payload_key = MakeContentUploadPayloadKey(key, plan.get());
	auto const payload = worker.FindPayload(payload_key);
	auto const reference_payload = reference_worker.FindPayload(payload_key);
	ASSERT_NE(nullptr, payload);
	ASSERT_NE(nullptr, reference_payload);
	EXPECT_EQ(reference_payload->primary, payload->primary);

	// Repainting the same viewport must reuse correct data without a zoom or reread.
	auto const reads = raw->FillCount();
	worker.Request(viewport, plan);
	ASSERT_TRUE(ready.WaitFor(3));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 2));
	EXPECT_EQ(reads, raw->FillCount());
	EXPECT_EQ(payload, worker.FindPayload(payload_key));
	worker.SetProvider(nullptr);
	reference_worker.SetProvider(nullptr);
}
}

TEST(skia_audio_content_worker, spectrum_scroll_recovers_partial_read_before_caching_payload) {
	ExpectSpectrumScrollRecoversPartialRead(false);
}

TEST(skia_audio_content_worker, spectrum_scroll_recovers_failed_ram_block_before_caching_payload) {
	ExpectSpectrumScrollRecoversPartialRead(true);
}

TEST(skia_audio_content_worker, spectrum_failed_reads_publish_no_tile_and_remain_retryable) {
	GateAudioProvider provider;
	ReadyLatch ready;
	ReadyLatch failure;
	ContentWorker worker(
		[&](ContentGeneration generation) { ready.Notify(generation); },
		[&](ContentWorkerFailure const&) { failure.Notify({}); });
	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(SpectrumAnalysis());
	auto const viewport = SpectrumViewport(generation, 128, 4);
	auto const key = FirstKey(viewport);
	auto const plan = SpectrumPlan();
	provider.FailNextFill(2);
	worker.Request(viewport, plan);
	ASSERT_TRUE(failure.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 0));
	EXPECT_EQ(2u, provider.FillCount());
	EXPECT_EQ(0u, worker.Metrics().ready_notifications);
	EXPECT_EQ(nullptr, worker.Find(key));
	EXPECT_EQ(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(key, plan.get())));

	worker.Request(viewport, plan);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 1));
	EXPECT_EQ(generation, worker.Generation());
	EXPECT_NE(nullptr, worker.Find(key));
	EXPECT_NE(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(key, plan.get())));
	worker.SetProvider(nullptr);
}

namespace {

void ExpectPrefetchFailureKeepsVisibleContent(ContentKind kind) {
	GateAudioProvider provider;
	GateAudioProvider reference_provider;
	ReadyLatch ready;
	ReadyLatch reference_ready;
	ReadyLatch failed;
	ContentWorkerFailure queued_failure;
	std::atomic<int> failure_count{0};
	bool prefetch_failure_armed = false;
	ContentWorker worker(
		[&](ContentGeneration generation) {
			// Visible content is already published when this callback runs.
			// Only its immediately following prefetch read should fail.
			if (!prefetch_failure_armed) {
				prefetch_failure_armed = true;
				provider.FailNextFill(2);
			}
			ready.Notify(generation);
		},
		[&](ContentWorkerFailure failure) {
			queued_failure = std::move(failure);
			++failure_count;
			failed.Notify({});
		});
	ContentWorker reference_worker([&](ContentGeneration generation) { reference_ready.Notify(generation); });
	worker.SetProvider(&provider);
	reference_worker.SetProvider(&reference_provider);
	auto analysis = kind == ContentKind::Waveform ? WaveformAnalysis() : SpectrumAnalysis();
	analysis.source_mode = ContentSourceMode::Int16Mono;
	auto const generation = worker.SetAnalysis(analysis);
	ASSERT_EQ(generation, reference_worker.SetAnalysis(analysis));
	ContentViewportRequest viewport;
	viewport.generation = generation;
	viewport.kind = kind;
	viewport.column_count = 256;
	viewport.tile_column_count = 256;
	viewport.spectrum_bin_count = kind == ContentKind::Spectrum ? 16 : 0;
	viewport.prefetch_tile_count = 1;
	auto reference_viewport = viewport;
	reference_viewport.prefetch_tile_count = 0;
	auto const plan = kind == ContentKind::Spectrum ? SpectrumPlan() : nullptr;
	worker.Request(viewport, plan);
	reference_worker.Request(reference_viewport, plan);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(reference_ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 1));
	ASSERT_TRUE(WaitForPayloadBuilds(reference_worker, 1));
	EXPECT_EQ(0, failure_count.load());
	EXPECT_EQ(1u, worker.Metrics().builds_invalid);
	EXPECT_EQ(reference_provider.FillCount() + 2, provider.FillCount());
	auto const key = FirstKey(viewport);
	auto const tile = worker.Find(key);
	auto const expected = reference_worker.Find(key);
	ASSERT_NE(nullptr, tile);
	ASSERT_NE(nullptr, expected);
	EXPECT_EQ(expected->waveform, tile->waveform);
	EXPECT_EQ(expected->spectrum_power, tile->spectrum_power);
	auto const payload_key = MakeContentUploadPayloadKey(key, plan.get());
	auto const payload = worker.FindPayload(payload_key);
	auto const expected_payload = reference_worker.FindPayload(payload_key);
	ASSERT_NE(nullptr, payload);
	ASSERT_NE(nullptr, expected_payload);
	EXPECT_EQ(expected_payload->primary, payload->primary);
	auto prefetched_key = key;
	++prefetched_key.tile_index;
	auto const prefetched_payload_key = MakeContentUploadPayloadKey(prefetched_key, plan.get());
	EXPECT_EQ(nullptr, worker.Find(prefetched_key));
	EXPECT_EQ(nullptr, worker.FindPayload(prefetched_payload_key));

	// Once that same region is visible, an exhausted retry must still report
	// its current error rather than silently leaving the viewport incomplete.
	viewport.first_column = 256;
	viewport.prefetch_tile_count = 0;
	provider.FailNextFill(2);
	auto const reads_before_visible_failure = provider.FillCount();
	worker.Request(viewport, plan);
	ASSERT_TRUE(failed.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 1));
	EXPECT_EQ(1, failure_count.load());
	EXPECT_EQ("injected partial audio read", queued_failure.message);
	EXPECT_TRUE(worker.IsFailureCurrent(queued_failure));
	EXPECT_EQ(reads_before_visible_failure + 2, provider.FillCount());
	EXPECT_EQ(nullptr, worker.Find(prefetched_key));
	EXPECT_EQ(nullptr, worker.FindPayload(prefetched_payload_key));
	EXPECT_EQ(tile, worker.Find(key));
	EXPECT_EQ(payload, worker.FindPayload(payload_key));

	// A later request recovers from fresh samples and caches exactly the same
	// tile and upload bytes as the healthy source, including rolling FFT reads.
	worker.Request(viewport, plan);
	reference_worker.Request(viewport, plan);
	ASSERT_TRUE(ready.WaitFor(2));
	ASSERT_TRUE(reference_ready.WaitFor(2));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 2));
	ASSERT_TRUE(WaitForPayloadBuilds(reference_worker, 2));
	EXPECT_EQ(1, failure_count.load());
	EXPECT_FALSE(worker.IsFailureCurrent(queued_failure));
	auto const recovered = worker.Find(prefetched_key);
	auto const reference = reference_worker.Find(prefetched_key);
	ASSERT_NE(nullptr, recovered);
	ASSERT_NE(nullptr, reference);
	EXPECT_EQ(reference->waveform, recovered->waveform);
	EXPECT_EQ(reference->spectrum_power, recovered->spectrum_power);
	auto const recovered_payload = worker.FindPayload(prefetched_payload_key);
	auto const reference_payload = reference_worker.FindPayload(prefetched_payload_key);
	ASSERT_NE(nullptr, recovered_payload);
	ASSERT_NE(nullptr, reference_payload);
	EXPECT_EQ(reference_payload->primary, recovered_payload->primary);
	auto const reads_after_recovery = provider.FillCount();
	worker.Request(viewport, plan);
	ASSERT_TRUE(ready.WaitFor(3));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 2));
	EXPECT_EQ(reads_after_recovery, provider.FillCount());
	EXPECT_EQ(recovered_payload, worker.FindPayload(prefetched_payload_key));
}

void ExpectObsoleteFailureSuppressed(bool change_analysis) {
	ObsoleteReadProvider provider;
	ReadyLatch ready;
	std::atomic<int> failure_count{0};
	ContentWorker worker(
		[&](ContentGeneration generation) { ready.Notify(generation); },
		[&](ContentWorkerFailure const&) { ++failure_count; });
	worker.SetProvider(&provider);
	auto const old_generation = worker.SetAnalysis(WaveformAnalysis());
	auto const old_viewport = WaveformViewport(old_generation, 0, 64);
	worker.Request(old_viewport);
	ASSERT_TRUE(provider.WaitUntilEntered());

	auto new_generation = old_generation;
	if (change_analysis) {
		auto analysis = WaveformAnalysis();
		analysis.milliseconds_per_pixel = 2.0;
		new_generation = worker.SetAnalysis(analysis);
		EXPECT_NE(old_generation, new_generation);
	}
	auto const new_viewport = WaveformViewport(new_generation, 64, 64);
	worker.Request(new_viewport);
	provider.Release();
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 1));
	EXPECT_FALSE(provider.GateTimedOut());
	EXPECT_EQ(2, provider.FailedReads());
	EXPECT_EQ(0, failure_count.load());
	EXPECT_EQ(1u, worker.Metrics().ready_notifications);
	EXPECT_EQ(nullptr, worker.Find(FirstKey(old_viewport)));
	EXPECT_EQ(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(FirstKey(old_viewport))));

	auto const key = FirstKey(new_viewport);
	auto const tile = worker.Find(key);
	ASSERT_NE(nullptr, tile);
	ASSERT_EQ(64u, tile->waveform.size());
	for (auto const& column : tile->waveform) {
		EXPECT_FLOAT_EQ(-0.25f, column.peak_min);
		EXPECT_FLOAT_EQ(0.5f, column.peak_max);
		EXPECT_FLOAT_EQ(-0.125f, column.average_min);
		EXPECT_FLOAT_EQ(0.25f, column.average_max);
	}
	auto const payload = worker.FindPayload(MakeContentUploadPayloadKey(key));
	ASSERT_NE(nullptr, payload);
	ASSERT_EQ(64u * kWaveformUploadBytesPerColumn, payload->primary.size());
	std::array<std::uint16_t, 4> const expected{24576, 49151, 28672, 40959};
	for (std::size_t column = 0; column < 64; ++column) {
		std::array<std::uint16_t, 4> endpoints;
		std::memcpy(endpoints.data(), payload->primary.data() + column * kWaveformUploadBytesPerColumn, sizeof(endpoints));
		EXPECT_EQ(expected, endpoints) << "column " << column;
	}
}

enum class FailureInvalidation : std::uint8_t {
	Viewport,
	Analysis,
	Provider,
};

void ExpectQueuedFailureInvalidated(FailureInvalidation invalidation) {
	GateAudioProvider provider;
	GateAudioProvider replacement;
	ReadyLatch ready;
	ReadyLatch failed;
	ContentWorkerFailure queued_failure;
	std::atomic<int> failure_count{0};
	ContentWorker worker(
		[&](ContentGeneration generation) { ready.Notify(generation); },
		[&](ContentWorkerFailure failure) {
			queued_failure = std::move(failure);
			++failure_count;
			failed.Notify({});
		});
	worker.SetProvider(&provider);
	auto generation = worker.SetAnalysis(WaveformAnalysis());
	provider.FailNextFill(2);
	worker.Request(WaveformViewport(generation, 0, 64));
	ASSERT_TRUE(failed.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 0));
	EXPECT_TRUE(worker.IsFailureCurrent(queued_failure));
	EXPECT_EQ("injected partial audio read", queued_failure.message);
	EXPECT_EQ(generation, queued_failure.generation);
	EXPECT_NE(0u, queued_failure.request_serial);
	EXPECT_EQ(2u, provider.FillCount());
	EXPECT_EQ(generation, worker.SetProvider(&provider));
	EXPECT_EQ(generation, worker.SetAnalysis(WaveformAnalysis()));
	EXPECT_TRUE(worker.IsFailureCurrent(queued_failure));

	switch (invalidation) {
		case FailureInvalidation::Viewport:
			break;
		case FailureInvalidation::Analysis: {
			auto analysis = WaveformAnalysis();
			analysis.milliseconds_per_pixel = 2.0;
			generation = worker.SetAnalysis(analysis);
			EXPECT_FALSE(worker.IsFailureCurrent(queued_failure));
			break;
		}
		case FailureInvalidation::Provider:
			worker.SetProvider(nullptr);
			EXPECT_FALSE(worker.IsFailureCurrent(queued_failure));
			generation = worker.SetProvider(&replacement);
			EXPECT_FALSE(worker.IsFailureCurrent(queued_failure));
			break;
	}

	auto const viewport = WaveformViewport(generation, 64, 64);
	worker.Request(viewport);
	EXPECT_FALSE(worker.IsFailureCurrent(queued_failure));
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 1));
	EXPECT_EQ(1, failure_count.load());
	EXPECT_NE(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(FirstKey(viewport))));
	EXPECT_FALSE(worker.IsFailureCurrent(queued_failure));
}

}

TEST(skia_audio_content_worker, waveform_prefetch_failure_preserves_visible_content_and_recovers) {
	ExpectPrefetchFailureKeepsVisibleContent(ContentKind::Waveform);
}

TEST(skia_audio_content_worker, spectrum_prefetch_failure_preserves_visible_content_and_recovers) {
	ExpectPrefetchFailureKeepsVisibleContent(ContentKind::Spectrum);
}

TEST(skia_audio_content_worker, obsolete_viewport_failure_does_not_interrupt_healthy_waveform) {
	ExpectObsoleteFailureSuppressed(false);
}

TEST(skia_audio_content_worker, obsolete_analysis_failure_does_not_interrupt_healthy_waveform) {
	ExpectObsoleteFailureSuppressed(true);
}

TEST(skia_audio_content_worker, queued_failure_is_rejected_after_viewport_changes) {
	ExpectQueuedFailureInvalidated(FailureInvalidation::Viewport);
}

TEST(skia_audio_content_worker, queued_failure_is_rejected_after_analysis_changes) {
	ExpectQueuedFailureInvalidated(FailureInvalidation::Analysis);
}

TEST(skia_audio_content_worker, queued_failure_is_rejected_after_provider_detach_and_attach) {
	ExpectQueuedFailureInvalidated(FailureInvalidation::Provider);
}

TEST(skia_audio_content_worker, audio_trace_records_spectrum_tile_lifecycle) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(
		path_helper.Decode("?temp/skia_audio_worker_trace_%%%%%%%%"));
	perf_trace::InitializeAt(session_dir, "test-build", "audio");

	bool ready_seen = false;
	bool build_finished = false;
	ContentTileKey key;
	{
		GateAudioProvider provider;
		ReadyLatch ready;
		ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });
		worker.SetProvider(&provider);
		auto const generation = worker.SetAnalysis(SpectrumAnalysis());
		auto const viewport = SpectrumViewport(generation, 0, 4);
		auto const band_plan = SpectrumPlan();
		key = FirstKey(viewport);
		worker.Request(viewport, band_plan);
		ready_seen = ready.WaitFor(1);
		build_finished = WaitForBuilds(worker, 1);
		worker.SetProvider(nullptr);
	}

	perf_trace::Shutdown();
	EXPECT_TRUE(ready_seen);
	EXPECT_TRUE(build_finished);
	auto const trace = ReadAll(session_dir / "trace.ndjson");
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"worker_build_start\""));
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"worker_build_end\""));
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"cpu_publish\""));
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"payload_build_start\""));
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"payload_build_end\""));
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"payload_publish\""));
	EXPECT_NE(std::string::npos, trace.find("\"content_kind\":\"spectrum\""));
	EXPECT_NE(std::string::npos, trace.find(
		"\"provider_generation\":" + std::to_string(key.generation.provider)));
	EXPECT_NE(std::string::npos, trace.find(
		"\"analysis_generation\":" + std::to_string(key.generation.analysis)));
	EXPECT_NE(std::string::npos, trace.find(
		"\"tile_index\":" + std::to_string(key.tile_index)));
	EXPECT_NE(std::string::npos, trace.find(
		"\"column_count\":" + std::to_string(key.column_count)));
	EXPECT_NE(std::string::npos, trace.find(
		"\"spectrum_bin_count\":" + std::to_string(key.spectrum_bin_count)));
	EXPECT_NE(std::string::npos, trace.find("\"visible\":true"));
	EXPECT_NE(std::string::npos, trace.find("\"fft_cache_hits_delta\":"));
	EXPECT_NE(std::string::npos, trace.find("\"fft_cache_misses_delta\":"));
	EXPECT_NE(std::string::npos, trace.find("\"fft_visible_builds_delta\":"));
	EXPECT_NE(std::string::npos, trace.find("\"fft_cache_evictions_delta\":"));
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

TEST(skia_audio_content_worker, waveform_visible_payload_cache_hit_notifies_after_prefetch_race) {
	ExpectVisiblePayloadCacheHitNotifiesAfterPrefetchRace(ContentKind::Waveform);
}

TEST(skia_audio_content_worker, spectrum_visible_payload_cache_hit_notifies_after_prefetch_race) {
	ExpectVisiblePayloadCacheHitNotifiesAfterPrefetchRace(ContentKind::Spectrum);
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

TEST(skia_audio_content_worker, budget_change_during_visible_build_skips_stale_prefetch) {
	GateAudioProvider provider(true);
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(WaveformAnalysis());
	auto viewport = WaveformViewport(generation, 64, 64);
	viewport.prefetch_tile_count = 1;
	auto const visible = FirstKey(viewport);
	auto const one_tile_budget = EstimateContentTileBytes(visible);
	ASSERT_GT(one_tile_budget, 0);

	worker.Request(viewport);
	ASSERT_TRUE(provider.WaitUntilEntered());
	auto updated = std::async(std::launch::async, [&] {
		return worker.SetCacheBudgets({
			one_tile_budget,
			kDefaultUploadPayloadCacheBudgetBytes,
			kMinimumSpectrumAnalysisBudgetBytes,
		});
	});
	EXPECT_EQ(updated.wait_for(200ms), std::future_status::ready);
	provider.Release();
	ASSERT_EQ(updated.wait_for(2s), std::future_status::ready);
	EXPECT_TRUE(updated.get());
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 1));

	auto before = visible;
	--before.tile_index;
	auto after = visible;
	++after.tile_index;
	EXPECT_NE(worker.Find(visible), nullptr);
	EXPECT_EQ(worker.Find(before), nullptr);
	EXPECT_EQ(worker.Find(after), nullptr);
	EXPECT_EQ(worker.Metrics().builds_started, 1u);
	EXPECT_EQ(worker.StoreMetrics().evictions, 0u);
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, cache_budget_update_preserves_generation_and_published_tiles) {
	GateAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(SpectrumAnalysis());
	EXPECT_TRUE(worker.SetCacheBudgets({ 1024 * 1024, 1024 * 1024, 64 * 1024 }));
	auto const viewport = SpectrumViewport(generation, 0, 4);
	auto const band_plan = SpectrumPlan();
	auto const key = FirstKey(viewport);
	worker.Request(viewport, band_plan);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForBuilds(worker, 1));
	ASSERT_TRUE(WaitForAnalysisBudget(worker, 64 * 1024));

	auto const tile = worker.Find(key);
	ASSERT_NE(tile, nullptr);
	auto const entries = worker.StoreMetrics().entries;
	auto const resets = worker.Metrics().analysis_resets;
	auto const builds = worker.Metrics().builds_started;
	EXPECT_TRUE(worker.SetCacheBudgets({ 2 * 1024 * 1024, 2 * 1024 * 1024, 32 * 1024 }));
	EXPECT_EQ(generation, worker.Generation());
	EXPECT_EQ(resets, worker.Metrics().analysis_resets);
	EXPECT_EQ(entries, worker.StoreMetrics().entries);
	EXPECT_EQ(tile.get(), worker.Find(key).get());
	ASSERT_TRUE(WaitForAnalysisBudget(worker, 32 * 1024));
	EXPECT_EQ(tile.get(), worker.Find(key).get());
	EXPECT_FALSE(worker.SetCacheBudgets({ 2 * 1024 * 1024, 2 * 1024 * 1024, 32 * 1024 }));

	worker.Request(viewport, band_plan);
	ASSERT_TRUE(WaitForBuilds(worker, 1));
	EXPECT_EQ(builds, worker.Metrics().builds_started);
	EXPECT_EQ(tile.get(), worker.Find(key).get());
	worker.SetProvider(nullptr);
}

TEST(skia_audio_content_worker, band_plan_change_reencodes_payload_without_rebuilding_raw_or_fft) {
	GateAudioProvider provider;
	ReadyLatch ready;
	ContentWorker worker([&](ContentGeneration generation) { ready.Notify(generation); });

	worker.SetProvider(&provider);
	auto const generation = worker.SetAnalysis(SpectrumAnalysis());
	auto const viewport = SpectrumViewport(generation, 0, 4);
	auto const key = FirstKey(viewport);
	auto const first_plan = SpectrumPlan(SpectrumScaleMode::LegacyLinear);
	worker.Request(viewport, first_plan);
	ASSERT_TRUE(ready.WaitFor(1));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 1));

	auto const raw = worker.Find(key);
	ASSERT_NE(nullptr, raw);
	auto const first_payload_key = MakeContentUploadPayloadKey(key, first_plan.get());
	ASSERT_NE(nullptr, worker.FindPayload(first_payload_key));
	auto const builds_before = worker.Metrics().builds_started;
	auto const fft_misses_before = worker.AnalysisMetrics().spectrum_cache_misses;

	auto const second_plan = SpectrumPlan(SpectrumScaleMode::FrequencyCurve);
	ASSERT_NE(first_plan->revision, second_plan->revision);
	worker.Request(viewport, second_plan);
	ASSERT_TRUE(ready.WaitFor(2));
	ASSERT_TRUE(WaitForPayloadBuilds(worker, 2));

	EXPECT_EQ(builds_before, worker.Metrics().builds_started);
	EXPECT_EQ(fft_misses_before, worker.AnalysisMetrics().spectrum_cache_misses);
	EXPECT_EQ(raw.get(), worker.Find(key).get());
	EXPECT_NE(nullptr, worker.FindPayload(first_payload_key));
	EXPECT_NE(nullptr, worker.FindPayload(MakeContentUploadPayloadKey(key, second_plan.get())));
	EXPECT_EQ(2u, worker.PayloadMetrics().entries);
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
