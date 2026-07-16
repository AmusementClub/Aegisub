#include <main.h>

#include "../../src/skia/audio/skia_audio_content.h"
#include "../../src/skia/audio/skia_audio_content_analysis.h"

#include "../../src/audio_display_source.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace audio = aegisub::skia::audio;

namespace {

class TestAudioSource final : public AudioDisplaySource {
	std::vector<float> samples;
	std::int64_t sample_count = 0;
	int channels = 1;
	int sample_rate = 1000;

public:
	mutable std::int64_t maximum_request = 0;
	mutable int request_count = 0;

	TestAudioSource(
		std::vector<float> samples,
		std::int64_t sample_count,
		int channels = 1,
		int sample_rate = 1000)
	: samples(std::move(samples))
	, sample_count(sample_count)
	, channels(channels)
	, sample_rate(sample_rate) {
	}

	std::int64_t GetNumSamples() const override { return sample_count; }
	int GetChannels() const override { return channels; }
	int GetSampleRate() const override { return sample_rate; }
	void GetFloatAudio(float *buffer, std::int64_t start, std::int64_t count) const override {
		++request_count;
		maximum_request = std::max(maximum_request, count);
		for (std::int64_t frame = 0; frame < count; ++frame) {
			for (int channel = 0; channel < channels; ++channel) {
				auto const source_frame = start + frame;
				auto const destination = static_cast<std::size_t>(frame) * channels + channel;
				if (source_frame < 0 || source_frame >= sample_count || samples.empty()) {
					buffer[destination] = 0.f;
					continue;
				}
				auto const source = static_cast<std::size_t>(source_frame) * channels + channel;
				buffer[destination] = source < samples.size() ? samples[source] : 0.f;
			}
		}
	}
};

std::shared_ptr<audio::ContentTile const> MakeWaveformTile(
	audio::ContentGeneration generation,
	std::uint64_t tile_index,
	std::uint32_t columns = 4) {
	auto tile = std::make_shared<audio::ContentTile>();
	tile->key = { generation, audio::ContentKind::Waveform, tile_index, columns, 0 };
	tile->waveform.resize(columns);
	for (std::uint32_t column = 0; column < columns; ++column) {
		auto const scale = static_cast<float>(column + 1) / columns;
		tile->waveform[column] = { -scale, scale, -scale * 0.5f, scale * 0.5f };
	}
	return tile;
}

std::shared_ptr<audio::ContentTile const> MakeSpectrumTile(
	audio::ContentGeneration generation,
	std::uint64_t tile_index,
	std::uint32_t columns = 3,
	std::uint32_t bins = 8) {
	auto tile = std::make_shared<audio::ContentTile>();
	tile->key = { generation, audio::ContentKind::Spectrum, tile_index, columns, bins };
	tile->spectrum_power.resize(static_cast<std::size_t>(columns) * bins);
	for (std::size_t i = 0; i < tile->spectrum_power.size(); ++i)
		tile->spectrum_power[i] = static_cast<float>(i) / tile->spectrum_power.size();
	return tile;
}

}

TEST(skia_audio_content, immutable_waveform_and_spectrum_tiles_validate_exact_shape) {
	audio::ContentGeneration const generation { 2, 4 };
	EXPECT_TRUE(MakeWaveformTile(generation, 0)->IsValid());
	EXPECT_TRUE(MakeSpectrumTile(generation, 1)->IsValid());

	auto invalid = std::make_shared<audio::ContentTile>();
	invalid->key = { generation, audio::ContentKind::Waveform, 0, 1, 0 };
	invalid->waveform.push_back({ 0.f, std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f });
	EXPECT_FALSE(invalid->IsValid());
}

TEST(skia_audio_content, generation_reset_drops_late_worker_result) {
	audio::ContentGeneration const old_generation { 1, 1 };
	audio::ContentGeneration const current_generation { 2, 1 };
	audio::ContentTileStore store;
	store.ResetGeneration(old_generation);
	auto late = MakeWaveformTile(old_generation, 0);

	store.ResetGeneration(current_generation);
	EXPECT_EQ(audio::ContentPublishResult::Stale, store.Publish(late));
	EXPECT_EQ(nullptr, store.Find(late->key));
	EXPECT_EQ(1, store.Metrics().stale_drops);
}

TEST(skia_audio_content, find_never_builds_missing_tile) {
	audio::ContentGeneration const generation { 3, 7 };
	audio::ContentTileStore store;
	store.ResetGeneration(generation);
	auto key = MakeSpectrumTile(generation, 8)->key;

	EXPECT_EQ(nullptr, store.Find(key));
	auto const metrics = store.Metrics();
	EXPECT_EQ(1, metrics.misses);
	EXPECT_EQ(0, metrics.publishes);
}

TEST(skia_audio_content, byte_budget_evicts_least_recently_used_tile) {
	audio::ContentGeneration const generation { 5, 9 };
	auto first = MakeWaveformTile(generation, 0);
	auto second = MakeWaveformTile(generation, 1);
	auto third = MakeWaveformTile(generation, 2);
	audio::ContentTileStore store(first->DataBytes() * 2);
	store.ResetGeneration(generation);

	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(first));
	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(second));
	ASSERT_NE(nullptr, store.Find(first->key));
	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(third));

	EXPECT_NE(nullptr, store.Find(first->key));
	EXPECT_EQ(nullptr, store.Find(second->key));
	EXPECT_NE(nullptr, store.Find(third->key));
	EXPECT_EQ(1, store.Metrics().evictions);
}

TEST(skia_audio_content, oversized_tile_is_rejected_without_evicting_hot_content) {
	audio::ContentGeneration const generation { 4, 3 };
	auto small = MakeWaveformTile(generation, 0, 2);
	auto large = MakeWaveformTile(generation, 1, 8);
	audio::ContentTileStore store(small->DataBytes());
	store.ResetGeneration(generation);

	ASSERT_EQ(audio::ContentPublishResult::Accepted, store.Publish(small));
	EXPECT_EQ(audio::ContentPublishResult::OverBudget, store.Publish(large));
	EXPECT_NE(nullptr, store.Find(small->key));
	EXPECT_EQ(1, store.Metrics().entries);
}

TEST(skia_audio_content, visible_plan_is_tile_aligned_and_overflow_safe) {
	audio::ContentViewportRequest request;
	request.generation = { 8, 13 };
	request.kind = audio::ContentKind::Spectrum;
	request.first_column = 63;
	request.column_count = 130;
	request.tile_column_count = 64;
	request.spectrum_bin_count = 256;

	auto const plan = audio::PlanVisibleContentTiles(request);
	ASSERT_EQ(4, plan.size());
	EXPECT_EQ(0, plan[0].tile_index);
	EXPECT_EQ(3, plan[3].tile_index);
	EXPECT_EQ(256, plan[2].spectrum_bin_count);

	request.first_column = std::numeric_limits<std::uint64_t>::max() - 4;
	request.column_count = 8;
	EXPECT_TRUE(audio::PlanVisibleContentTiles(request).empty());

	request.first_column = 0;
	request.column_count = std::numeric_limits<std::uint32_t>::max();
	request.tile_column_count = 1;
	EXPECT_TRUE(audio::PlanVisibleContentTiles(request).empty());
}

TEST(skia_audio_content, concurrent_late_publish_observes_new_generation) {
	audio::ContentGeneration const old_generation { 10, 1 };
	audio::ContentGeneration const new_generation { 11, 1 };
	audio::ContentTileStore store;
	store.ResetGeneration(old_generation);
	auto late = MakeSpectrumTile(old_generation, 0);
	audio::ContentPublishResult result = audio::ContentPublishResult::Accepted;

	store.ResetGeneration(new_generation);
	std::thread worker([&] { result = store.Publish(late); });
	worker.join();
	EXPECT_EQ(audio::ContentPublishResult::Stale, result);
}

TEST(skia_audio_content, waveform_builder_matches_min_max_and_signed_average_semantics) {
	TestAudioSource source({
		-1.f, -0.5f, 0.5f, 1.f,
		-0.25f, 0.25f, -0.75f, 0.75f,
	}, 8);
	audio::WaveformBuildRequest request;
	request.key = { { 1, 2 }, audio::ContentKind::Waveform, 0, 2, 0 };
	request.milliseconds_per_pixel = 4.0;
	request.mix_policy = AudioMixPolicy::MonoAverage;

	audio::ContentAnalyzer analyzer(source);
	auto const result = analyzer.BuildWaveform(request);
	ASSERT_EQ(audio::ContentBuildStatus::Ready, result.status);
	ASSERT_NE(nullptr, result.tile);
	ASSERT_EQ(2, result.tile->waveform.size());
	EXPECT_FLOAT_EQ(-1.f, result.tile->waveform[0].peak_min);
	EXPECT_FLOAT_EQ(1.f, result.tile->waveform[0].peak_max);
	EXPECT_FLOAT_EQ(-0.375f, result.tile->waveform[0].average_min);
	EXPECT_FLOAT_EQ(0.375f, result.tile->waveform[0].average_max);
	EXPECT_FLOAT_EQ(-0.75f, result.tile->waveform[1].peak_min);
	EXPECT_FLOAT_EQ(0.75f, result.tile->waveform[1].peak_max);
}

TEST(skia_audio_content, waveform_builder_bounds_temporary_audio_reads) {
	TestAudioSource source({}, 2000000);
	audio::WaveformBuildRequest request;
	request.key = { { 3, 4 }, audio::ContentKind::Waveform, 0, 1, 0 };
	request.milliseconds_per_pixel = 2000000.0;

	audio::ContentAnalyzer analyzer(source);
	auto const result = analyzer.BuildWaveform(request);
	ASSERT_EQ(audio::ContentBuildStatus::Ready, result.status);
	EXPECT_LE(source.maximum_request, 1024 * 1024 / static_cast<int>(sizeof(float)));
}

TEST(skia_audio_content, waveform_builder_observes_generation_cancellation) {
	TestAudioSource source({}, 1024);
	audio::WaveformBuildRequest request;
	request.key = { { 5, 6 }, audio::ContentKind::Waveform, 0, 8, 0 };
	request.milliseconds_per_pixel = 4.0;
	int checks = 0;

	audio::ContentAnalyzer analyzer(source);
	auto const result = analyzer.BuildWaveform(request, [&](audio::ContentGeneration) {
		return ++checks < 3;
	});
	EXPECT_EQ(audio::ContentBuildStatus::Cancelled, result.status);
	EXPECT_EQ(nullptr, result.tile);
}

TEST(skia_audio_content, spectrum_builder_copies_raw_fft_power_without_palette_or_height) {
	std::vector<float> samples(256);
	for (std::size_t i = 0; i < samples.size(); ++i)
		samples[i] = std::sin(static_cast<float>(i) * 0.31f);
	TestAudioSource source(samples, static_cast<std::int64_t>(samples.size()));
	audio::SpectrumBuildRequest request;
	request.key = { { 9, 10 }, audio::ContentKind::Spectrum, 0, 4, 32 };
	request.milliseconds_per_pixel = 8.0;
	request.derivation_size = 5;
	request.derivation_distance = 3;

	audio::ContentAnalyzer analyzer(source);
	auto const result = analyzer.BuildSpectrum(request);
	ASSERT_EQ(audio::ContentBuildStatus::Ready, result.status);
	ASSERT_NE(nullptr, result.tile);
	ASSERT_EQ(128, result.tile->spectrum_power.size());

	bool any_power = false;
	for (std::size_t bin = 0; bin < 32; ++bin) {
		EXPECT_TRUE(std::isfinite(result.tile->spectrum_power[bin]));
		any_power = any_power || result.tile->spectrum_power[bin] != 0.f;
	}
	EXPECT_TRUE(any_power);
	EXPECT_NE(result.tile->spectrum_power[0], result.tile->spectrum_power[32]);
	auto const requests_after_first = source.request_count;
	auto const repeated = analyzer.BuildSpectrum(request);
	EXPECT_EQ(audio::ContentBuildStatus::Ready, repeated.status);
	EXPECT_EQ(requests_after_first, source.request_count);
}

TEST(skia_audio_content, spectrum_builder_observes_generation_cancellation) {
	TestAudioSource source({}, 1024);
	audio::SpectrumBuildRequest request;
	request.key = { { 12, 13 }, audio::ContentKind::Spectrum, 0, 4, 32 };
	request.milliseconds_per_pixel = 4.0;
	request.derivation_size = 5;
	request.derivation_distance = 3;

	audio::ContentAnalyzer analyzer(source);
	auto const result = analyzer.BuildSpectrum(request, [](audio::ContentGeneration) {
		return false;
	});
	EXPECT_EQ(audio::ContentBuildStatus::Cancelled, result.status);
	EXPECT_EQ(nullptr, result.tile);
}

TEST(skia_audio_content, spectrum_builder_rejects_transform_sizes_below_pffft_floor) {
	TestAudioSource source({}, 1024);
	audio::SpectrumBuildRequest request;
	request.key = { { 14, 15 }, audio::ContentKind::Spectrum, 0, 4, 8 };
	request.milliseconds_per_pixel = 4.0;
	request.derivation_size = 3;
	request.derivation_distance = 2;

	audio::ContentAnalyzer analyzer(source);
	auto const result = analyzer.BuildSpectrum(request);
	EXPECT_EQ(audio::ContentBuildStatus::InvalidRequest, result.status);
	EXPECT_EQ(nullptr, result.tile);
}

TEST(skia_audio_content, spectrum_builder_supports_per_channel_power_aggregation) {
	std::vector<float> samples(64 * 2, 0.f);
	for (std::size_t frame = 0; frame < 64; ++frame)
		samples[frame * 2 + 1] = 1.f;
	TestAudioSource source(std::move(samples), 64, 2, 1000);

	audio::SpectrumBuildRequest request;
	request.key = { { 13, 2 }, audio::ContentKind::Spectrum, 0, 1, 16 };
	request.milliseconds_per_pixel = 16.0;
	request.derivation_size = 4;
	request.derivation_distance = 4;
	request.channel_mode = audio::SpectrumChannelMode::PerBinMaxPower;
	audio::ContentAnalyzer analyzer(source);
	auto const max_result = analyzer.BuildSpectrum(request);
	ASSERT_EQ(audio::ContentBuildStatus::Ready, max_result.status);
	ASSERT_NE(max_result.tile, nullptr);

	request.key.generation.analysis = 3;
	request.channel_mode = audio::SpectrumChannelMode::PerBinAveragePower;
	auto const average_result = analyzer.BuildSpectrum(request);
	ASSERT_EQ(audio::ContentBuildStatus::Ready, average_result.status);
	ASSERT_NE(average_result.tile, nullptr);
	EXPECT_GT(max_result.tile->spectrum_power[0], 0.f);
	EXPECT_GT(max_result.tile->spectrum_power[0], average_result.tile->spectrum_power[0]);
}
