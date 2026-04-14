// Copyright (c) 2026

#include <main.h>

#include "../../src/audio_display_analysis.h"
#include "../../src/audio_latest_range_scheduler.h"
#include "../../src/audio_display_source.h"
#include "../../src/audio_mix_policy.h"
#include "../../src/audio_spectrum_analysis_cache.h"
#include "../../src/audio_waveform_summary_cache.h"

#include <libaegisub/audio/provider.h>

#include <chrono>
#include <cmath>
#include <thread>

namespace {
struct Int16StereoProvider final : agi::AudioProvider {
	Int16StereoProvider() {
		channels = 2;
		num_samples = 4;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const int16_t samples[] = {
			32767, -32768,
			16384, -16384,
			8192, -8192,
			4096, -4096,
		};
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = samples[(start + i) * 2 + 0];
			out[i * 2 + 1] = samples[(start + i) * 2 + 1];
		}
	}
};

struct FloatStereoProvider final : agi::AudioProvider {
	FloatStereoProvider() {
		channels = 2;
		num_samples = 2;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(float);
		float_samples = true;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const float samples[] = {
			0.25f, -0.5f,
			0.75f, -1.0f,
		};
		auto out = static_cast<float *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = samples[(start + i) * 2 + 0];
			out[i * 2 + 1] = samples[(start + i) * 2 + 1];
		}
	}
};

struct CountingStereoProvider final : agi::AudioProvider {
	mutable int fill_calls = 0;

	CountingStereoProvider() {
		channels = 2;
		num_samples = 1 << 21;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		++fill_calls;
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = static_cast<int16_t>(((start + i) * 17) % 32767);
			out[i * 2 + 1] = static_cast<int16_t>(-(((start + i) * 29) % 32768));
		}
	}
};
}

TEST(lagi_audio_display, display_source_converts_s16_stereo_to_float) {
	Int16StereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);
	EXPECT_EQ(2, source->GetChannels());
	EXPECT_EQ(48000, source->GetSampleRate());

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(32767.0f / 32768.0f, samples[0], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[1], 1e-6f);
	EXPECT_NEAR(16384.0f / 32768.0f, samples[2], 1e-6f);
	EXPECT_NEAR(-16384.0f / 32768.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, display_source_preserves_float_samples) {
	FloatStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(0.25f, samples[0], 1e-6f);
	EXPECT_NEAR(-0.5f, samples[1], 1e-6f);
	EXPECT_NEAR(0.75f, samples[2], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, single_channel_display_source_extracts_requested_s16_channel) {
	Int16StereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	auto right = CreateSingleChannelAudioDisplaySource(source.get(), 1);
	ASSERT_TRUE(!!right);

	float samples[2] = { 0.f, 0.f };
	right->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(-1.0f, samples[0], 1e-6f);
	EXPECT_NEAR(-16384.0f / 32768.0f, samples[1], 1e-6f);
}

TEST(lagi_audio_display, single_channel_display_source_extracts_requested_float_channel) {
	FloatStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	auto left = CreateSingleChannelAudioDisplaySource(source.get(), 0);
	ASSERT_TRUE(!!left);

	float samples[2] = { 0.f, 0.f };
	left->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(0.25f, samples[0], 1e-6f);
	EXPECT_NEAR(0.75f, samples[1], 1e-6f);
}

TEST(lagi_audio_display, mix_policy_average_and_maxabs) {
	const float frame[] = { 0.25f, -0.75f, 0.5f };
	EXPECT_NEAR(0.0f, MixAudioFrameToMono(AudioMixPolicy::MonoAverage, frame, 3), 1e-6f);
	EXPECT_NEAR(-0.75f, MixAudioFrameToMono(AudioMixPolicy::MonoMaxAbs, frame, 3), 1e-6f);
}

TEST(lagi_audio_display, waveform_analysis_uses_requested_mix_policy) {
	const float interleaved[] = {
		0.25f, -0.5f,
		0.5f, -0.25f,
		-0.75f, 0.25f,
		0.1f, -0.9f,
	};

	auto average = AnalyzeWaveformInterleaved(interleaved, 4, 2, AudioMixPolicy::MonoAverage);
	auto maxabs = AnalyzeWaveformInterleaved(interleaved, 4, 2, AudioMixPolicy::MonoMaxAbs);

	EXPECT_LT(average.peak_max, maxabs.peak_max);
	EXPECT_GT(average.peak_min, maxabs.peak_min);
	EXPECT_NEAR(-0.9f, maxabs.peak_min, 1e-6f);
	EXPECT_NEAR(0.5f, maxabs.peak_max, 1e-6f);
}

TEST(lagi_audio_display, spectrum_power_merge_max_uses_strongest_channel_per_bin) {
	const float left[] = { 0.10f, 0.80f, 0.30f, 0.20f };
	const float right[] = { 0.50f, 0.20f, 0.70f, 0.40f };
	const std::vector<const float *> channels = { left, right };
	float merged[4] = { 0.f, 0.f, 0.f, 0.f };

	MergeSpectrumPowerBinsMax(channels, 4, merged);

	EXPECT_NEAR(0.50f, merged[0], 1e-6f);
	EXPECT_NEAR(0.80f, merged[1], 1e-6f);
	EXPECT_NEAR(0.70f, merged[2], 1e-6f);
	EXPECT_NEAR(0.40f, merged[3], 1e-6f);
}

TEST(lagi_audio_display, spectrum_power_merge_average_averages_each_bin) {
	const float left[] = { 0.10f, 0.80f, 0.30f, 0.20f };
	const float right[] = { 0.50f, 0.20f, 0.70f, 0.40f };
	const float center[] = { 0.40f, 0.10f, 0.20f, 0.90f };
	const std::vector<const float *> channels = { left, right, center };
	float merged[4] = { 0.f, 0.f, 0.f, 0.f };

	MergeSpectrumPowerBinsAverage(channels, 4, merged);

	EXPECT_NEAR((0.10f + 0.50f + 0.40f) / 3.0f, merged[0], 1e-6f);
	EXPECT_NEAR((0.80f + 0.20f + 0.10f) / 3.0f, merged[1], 1e-6f);
	EXPECT_NEAR((0.30f + 0.70f + 0.20f) / 3.0f, merged[2], 1e-6f);
	EXPECT_NEAR((0.20f + 0.40f + 0.90f) / 3.0f, merged[3], 1e-6f);
}

TEST(lagi_audio_display, waveform_summary_cache_reuses_hot_block) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	auto first = cache.Get(0);
	int calls_after_first = provider.fill_calls;
	auto second = cache.Get(0);

	EXPECT_EQ(1, calls_after_first);
	EXPECT_EQ(calls_after_first, provider.fill_calls);
	EXPECT_EQ(first.summaries[0].peak_max, second.summaries[0].peak_max);
}

TEST(lagi_audio_display, waveform_summary_cache_batches_adjacent_visible_blocks) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	const int calls_after_first = provider.fill_calls;
	cache.Get(1);

	EXPECT_EQ(1, calls_after_first);
	EXPECT_EQ(calls_after_first, provider.fill_calls);
}

TEST(lagi_audio_display, waveform_summary_cache_invalidates_on_zoom_change) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	int calls_after_first = provider.fill_calls;
	cache.SetMillisecondsPerPixel(10.0);
	cache.Get(0);

	EXPECT_GT(provider.fill_calls, calls_after_first);
}

TEST(lagi_audio_display, waveform_summary_cache_metrics_count_hits_and_misses) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	cache.Get(0);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.cache_misses);
	EXPECT_EQ(1u, metrics.cache_hits);
	EXPECT_EQ(1u, metrics.visible_builds);
}

TEST(lagi_audio_display, waveform_summary_cache_prefetch_records_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	cache.Prefetch(40, 41);
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	cache.Get(40);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_GE(metrics.prefetch_requests, 1u);
	EXPECT_GE(metrics.prefetch_builds, 1u);
}

TEST(lagi_audio_display, waveform_summary_cache_get_if_ready_becomes_available_after_prefetch) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(0, provider.fill_calls);

	cache.Prefetch(0, 0);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	const AudioWaveformSummaryBlock *block = nullptr;
	while (!(block = cache.GetIfReady(0)) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	ASSERT_NE(nullptr, block);
	EXPECT_GT(provider.fill_calls, 0);
}

TEST(lagi_audio_display, waveform_summary_cache_get_if_ready_does_not_mutate_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	auto const before = cache.GetMetricsSnapshot();
	EXPECT_NE(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(nullptr, cache.GetIfReady(40));
	auto const after = cache.GetMetricsSnapshot();

	EXPECT_EQ(before.cache_hits, after.cache_hits);
	EXPECT_EQ(before.cache_misses, after.cache_misses);
}

TEST(lagi_audio_display, latest_range_scheduler_request_increments_generation) {
	AudioLatestRangeScheduler scheduler([](size_t, size_t, uint64_t) {});
	const uint64_t before = scheduler.CurrentGeneration();
	scheduler.Request(0, 1);
	const uint64_t after = scheduler.CurrentGeneration();

	EXPECT_GT(after, before);
	EXPECT_TRUE(scheduler.IsCurrent(after));
	EXPECT_FALSE(scheduler.IsCurrent(before));
}

TEST(lagi_audio_display, latest_range_scheduler_invalidate_bumps_generation) {
	AudioLatestRangeScheduler scheduler([](size_t, size_t, uint64_t) {});
	scheduler.Request(2, 3);
	const uint64_t after_request = scheduler.CurrentGeneration();
	scheduler.Invalidate();
	const uint64_t after_invalidate = scheduler.CurrentGeneration();

	EXPECT_GT(after_invalidate, after_request);
	EXPECT_TRUE(scheduler.IsCurrent(after_invalidate));
	EXPECT_FALSE(scheduler.IsCurrent(after_request));
}

TEST(lagi_audio_display, spectrum_analysis_cache_reuses_hot_block) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	const float *first = cache.Get(0);
	int calls_after_first = provider.fill_calls;
	const float *second = cache.Get(0);

	EXPECT_GT(calls_after_first, 0);
	EXPECT_EQ(calls_after_first, provider.fill_calls);
	EXPECT_EQ(first[0], second[0]);
}

TEST(lagi_audio_display, spectrum_analysis_cache_batches_adjacent_visible_blocks) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	const int calls_after_first = provider.fill_calls;
	cache.Get(1);

	EXPECT_EQ(1, calls_after_first);
	EXPECT_EQ(calls_after_first, provider.fill_calls);
}

TEST(lagi_audio_display, spectrum_analysis_cache_metrics_count_hits_and_misses) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	cache.Get(0);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.cache_misses);
	EXPECT_EQ(1u, metrics.cache_hits);
	EXPECT_EQ(1u, metrics.visible_builds);
}

TEST(lagi_audio_display, spectrum_analysis_cache_prefetch_records_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	cache.Prefetch(300, 301);
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	cache.Get(300);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_GE(metrics.prefetch_requests, 2u);
	EXPECT_GE(metrics.prefetch_builds, 1u);
}

TEST(lagi_audio_display, spectrum_analysis_cache_get_if_ready_becomes_available_after_prefetch) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(0, provider.fill_calls);

	cache.Prefetch(0, 0);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	const float *block = nullptr;
	while (!(block = cache.GetIfReady(0)) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	ASSERT_NE(nullptr, block);
	EXPECT_GT(provider.fill_calls, 0);
}

TEST(lagi_audio_display, spectrum_analysis_cache_get_if_ready_does_not_mutate_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	auto const before = cache.GetMetricsSnapshot();
	EXPECT_NE(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(nullptr, cache.GetIfReady(300));
	auto const after = cache.GetMetricsSnapshot();

	EXPECT_EQ(before.cache_hits, after.cache_hits);
	EXPECT_EQ(before.cache_misses, after.cache_misses);
}

TEST(lagi_audio_display, spectrum_analysis_cache_stays_finite_with_prefetch_interleaving) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	for (size_t i = 0; i < 32; ++i) {
		cache.Prefetch(i + 1, i + 4);
		const float *block = cache.Get(i);
		for (size_t b = 0; b < 32; ++b) {
			EXPECT_TRUE(std::isfinite(block[b]));
		}
	}
}

TEST(lagi_audio_display, track_cursor_overlay_refresh_policy_handles_same_pixel_updates) {
	EXPECT_FALSE(ShouldRefreshTrackCursor(-1, -1));
	EXPECT_TRUE(ShouldRefreshTrackCursor(-1, 120));
	EXPECT_TRUE(ShouldRefreshTrackCursor(120, -1));
	EXPECT_TRUE(ShouldRefreshTrackCursor(120, 120));
	EXPECT_TRUE(ShouldRefreshTrackCursor(120, 121));
}
