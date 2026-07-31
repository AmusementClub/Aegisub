#include <main.h>

#include "../../src/skia/audio/skia_audio_content.h"
#include "../../src/skia/audio/skia_audio_content_analysis.h"
#include "../../src/skia/audio/skia_audio_upload_payload.h"

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

audio::ContentViewportRequest HighestQualitySpectrumViewport(
	std::uint64_t first_column,
	std::uint32_t column_count,
	std::uint32_t prefetch_tile_count = 1) {
	audio::ContentViewportRequest request;
	request.generation = { 21, 34 };
	request.kind = audio::ContentKind::Spectrum;
	request.first_column = first_column;
	request.column_count = column_count;
	request.tile_column_count = 256;
	request.spectrum_bin_count = 2048;
	request.prefetch_tile_count = prefetch_tile_count;
	return request;
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

TEST(skia_audio_content, waveform_upload_payload_builds_exact_peak_and_average_masks) {
	audio::ContentGeneration const generation { 2, 4 };
	auto tile = MakeWaveformTile(generation, 0, 1);
	auto const built = audio::BuildWaveformUploadPayload(*tile);
	ASSERT_EQ(audio::ContentUploadPayloadBuildStatus::Ready, built.status);
	ASSERT_NE(nullptr, built.payload);
	EXPECT_TRUE(built.payload->IsValid());
	EXPECT_EQ(audio::kWaveformUploadPayloadRevision, built.payload->key.variant_revision);
	EXPECT_EQ(1u, built.payload->width);
	EXPECT_EQ(audio::kWaveformUploadMaskHeight, built.payload->height);
	ASSERT_EQ(audio::kWaveformUploadMaskHeight, built.payload->primary.size());
	ASSERT_EQ(audio::kWaveformUploadMaskHeight, built.payload->secondary.size());
	EXPECT_EQ(255, built.payload->primary.front());
	EXPECT_EQ(255, built.payload->primary.back());
	EXPECT_EQ(0, built.payload->secondary.front());
	EXPECT_EQ(0, built.payload->secondary.back());
	EXPECT_EQ(255, built.payload->secondary[audio::kWaveformUploadMaskHeight / 2]);

	auto const cancelled = audio::BuildWaveformUploadPayload(
		*tile,
		[](audio::ContentGeneration) { return false; });
	EXPECT_EQ(audio::ContentUploadPayloadBuildStatus::Cancelled, cancelled.status);
	EXPECT_EQ(nullptr, cancelled.payload);
}

TEST(skia_audio_content, spectrum_upload_payload_is_revisioned_without_mutating_raw_power) {
	audio::ContentGeneration const generation { 3, 5 };
	auto tile = MakeSpectrumTile(generation, 7, 2, 8);
	auto const raw_power = tile->spectrum_power;

	audio::SpectrumBandPlanRequest request;
	request.bin_count = 8;
	request.output_height = 4;
	request.sample_rate = 48000;
	request.mode = audio::SpectrumScaleMode::LegacyLinear;
	auto first_plan = audio::BuildSpectrumBandPlan(request);
	ASSERT_TRUE(first_plan.IsValid());
	auto first = audio::BuildSpectrumUploadPayload(*tile, first_plan);
	ASSERT_EQ(audio::ContentUploadPayloadBuildStatus::Ready, first.status);
	ASSERT_NE(nullptr, first.payload);
	EXPECT_TRUE(first.payload->IsValid());
	EXPECT_EQ(first_plan.revision, first.payload->key.variant_revision);
	EXPECT_EQ(2u, first.payload->width);
	EXPECT_EQ(4u, first.payload->height);
	EXPECT_EQ(2u * 4u * 4u, first.payload->primary.size());
	EXPECT_TRUE(first.payload->secondary.empty());

	request.mode = audio::SpectrumScaleMode::FrequencyCurve;
	request.frequency_reference_position = 0.425f;
	auto second_plan = audio::BuildSpectrumBandPlan(request);
	ASSERT_TRUE(second_plan.IsValid());
	ASSERT_NE(first_plan.revision, second_plan.revision);
	auto second = audio::BuildSpectrumUploadPayload(*tile, second_plan);
	ASSERT_EQ(audio::ContentUploadPayloadBuildStatus::Ready, second.status);
	ASSERT_NE(nullptr, second.payload);
	EXPECT_NE(first.payload->key, second.payload->key);
	EXPECT_EQ(raw_power, tile->spectrum_power);
}

TEST(skia_audio_content, upload_payload_store_is_generation_safe_and_budgeted_by_variant) {
	audio::ContentGeneration const generation { 4, 6 };
	auto tile = MakeSpectrumTile(generation, 0, 2, 8);
	audio::SpectrumBandPlanRequest request;
	request.bin_count = 8;
	request.output_height = 4;
	request.sample_rate = 48000;
	auto first = audio::BuildSpectrumUploadPayload(
		*tile,
		audio::BuildSpectrumBandPlan(request)).payload;
	ASSERT_NE(nullptr, first);

	request.frequency_reference_position = 0.425f;
	auto second = audio::BuildSpectrumUploadPayload(
		*tile,
		audio::BuildSpectrumBandPlan(request)).payload;
	ASSERT_NE(nullptr, second);
	ASSERT_NE(first->key, second->key);

	audio::ContentUploadPayloadStore store(first->DataBytes());
	store.ResetGeneration(generation);
	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(first));
	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(second));
	EXPECT_EQ(nullptr, store.Find(first->key));
	EXPECT_NE(nullptr, store.Find(second->key));
	EXPECT_EQ(1u, store.Metrics().entries);
	EXPECT_EQ(1u, store.Metrics().evictions);

	store.ResetGeneration({ generation.provider + 1, generation.analysis });
	EXPECT_EQ(audio::ContentPublishResult::Stale, store.Publish(second));
	EXPECT_EQ(nullptr, store.Find(second->key));
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
	std::vector<std::pair<audio::ContentTileKey, std::size_t>> evictions;
	audio::ContentTileStore store(
		first->DataBytes() * 2,
		[&](audio::ContentTileKey const& key, std::size_t bytes) {
			evictions.emplace_back(key, bytes);
		});
	store.ResetGeneration(generation);

	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(first));
	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(second));
	ASSERT_NE(nullptr, store.Find(first->key));
	EXPECT_EQ(audio::ContentPublishResult::Accepted, store.Publish(third));

	EXPECT_NE(nullptr, store.Find(first->key));
	EXPECT_EQ(nullptr, store.Find(second->key));
	EXPECT_NE(nullptr, store.Find(third->key));
	EXPECT_EQ(1, store.Metrics().evictions);
	ASSERT_EQ(1, evictions.size());
	EXPECT_EQ(second->key, evictions.front().first);
	EXPECT_EQ(second->DataBytes(), evictions.front().second);
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

TEST(skia_audio_content, four_k_highest_quality_budget_retains_aligned_and_unaligned_working_sets) {
	constexpr std::uint32_t output_height = 512;
	auto aligned = HighestQualitySpectrumViewport(256, 3840);
	auto const aligned_tiles = audio::PlanVisibleContentTiles(aligned);
	ASSERT_EQ(15, aligned_tiles.size());
	EXPECT_EQ(1, aligned_tiles.front().tile_index);
	EXPECT_EQ(15, aligned_tiles.back().tile_index);

	auto unaligned = HighestQualitySpectrumViewport(257, 3840);
	auto const visible_tiles = audio::PlanVisibleContentTiles(unaligned);
	ASSERT_EQ(16, visible_tiles.size());
	EXPECT_EQ(1, visible_tiles.front().tile_index);
	EXPECT_EQ(16, visible_tiles.back().tile_index);

	auto const tile_bytes = audio::EstimateContentTileBytes(visible_tiles.front());
	ASSERT_GT(tile_bytes, 0);
	auto const payload_tile_bytes = audio::EstimateContentUploadPayloadBytes(
		visible_tiles.front(),
		output_height);
	ASSERT_GT(payload_tile_bytes, 0);
	auto const constrained = audio::PlanContentCacheBudget(
		unaligned,
		2 * 1024 * 1024,
		output_height);
	ASSERT_TRUE(constrained.valid);
	EXPECT_TRUE(constrained.soft_limit_exceeded);
	EXPECT_EQ(tile_bytes * visible_tiles.size(), constrained.visible_bytes);
	EXPECT_EQ(
		payload_tile_bytes * visible_tiles.size(),
		constrained.payload_visible_bytes);
	EXPECT_EQ(constrained.visible_bytes, constrained.content_budget_bytes);
	EXPECT_EQ(constrained.payload_visible_bytes, constrained.payload_budget_bytes);
	EXPECT_EQ(audio::kMinimumSpectrumAnalysisBudgetBytes,
		constrained.spectrum_analysis_budget_bytes);
	EXPECT_EQ(0, constrained.prefetch_bytes);
	EXPECT_EQ(0, constrained.payload_prefetch_bytes);

	auto const configured = audio::PlanContentCacheBudget(
		unaligned,
		128 * 1024 * 1024,
		output_height);
	ASSERT_TRUE(configured.valid);
	EXPECT_FALSE(configured.soft_limit_exceeded);
	EXPECT_EQ(tile_bytes * 2, configured.prefetch_bytes);
	EXPECT_EQ(payload_tile_bytes * 2, configured.payload_prefetch_bytes);
	EXPECT_EQ(configured.visible_bytes + configured.prefetch_bytes,
		configured.content_budget_bytes);
	EXPECT_EQ(configured.payload_visible_bytes + configured.payload_prefetch_bytes,
		configured.payload_budget_bytes);
	EXPECT_EQ(configured.effective_total_bytes,
		configured.content_budget_bytes
			+ configured.payload_budget_bytes
			+ configured.spectrum_analysis_budget_bytes);
	auto const partial_prefetch = audio::PlanContentCacheBudget(
		unaligned,
		constrained.visible_bytes
			+ constrained.payload_visible_bytes
			+ audio::kMinimumSpectrumAnalysisBudgetBytes
			+ (tile_bytes + payload_tile_bytes) / 2,
		output_height);
	ASSERT_TRUE(partial_prefetch.valid);
	EXPECT_EQ(0, partial_prefetch.prefetch_bytes);
	EXPECT_EQ(0, partial_prefetch.payload_prefetch_bytes);

	audio::ContentTileStore store(constrained.content_budget_bytes);
	store.ResetGeneration(unaligned.generation);
	for (auto const& key : visible_tiles) {
		auto tile = MakeSpectrumTile(
			key.generation,
			key.tile_index,
			key.column_count,
			key.spectrum_bin_count);
		ASSERT_EQ(tile_bytes, tile->DataBytes());
		ASSERT_EQ(audio::ContentPublishResult::Accepted, store.Publish(std::move(tile)));
	}
	EXPECT_EQ(visible_tiles.size(), store.Metrics().entries);
	EXPECT_EQ(0, store.Metrics().evictions);
	for (auto const& key : visible_tiles)
		EXPECT_NE(nullptr, store.Find(key));
}

TEST(skia_audio_content, cache_budget_planning_rejects_invalid_extreme_shapes_without_wrapping) {
	auto request = HighestQualitySpectrumViewport(0, 3840);
	request.spectrum_bin_count = std::numeric_limits<std::uint32_t>::max();
	EXPECT_FALSE(audio::PlanContentCacheBudget(
		request,
		std::numeric_limits<std::size_t>::max(),
		512).valid);
	EXPECT_EQ(0, audio::EstimateContentTileBytes({
		request.generation,
		audio::ContentKind::Spectrum,
		0,
		request.tile_column_count,
		request.spectrum_bin_count,
	}));

	request = HighestQualitySpectrumViewport(0, 3840);
	auto const maximum = audio::PlanContentCacheBudget(
		request,
		std::numeric_limits<std::size_t>::max(),
		512);
	ASSERT_TRUE(maximum.valid);
	EXPECT_EQ(std::numeric_limits<std::size_t>::max(), maximum.effective_total_bytes);
	EXPECT_EQ(maximum.effective_total_bytes,
		maximum.content_budget_bytes
			+ maximum.payload_budget_bytes
			+ maximum.spectrum_analysis_budget_bytes);
	EXPECT_FALSE(audio::PlanContentCacheBudget(request, 128 * 1024 * 1024, 0).valid);
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
	constexpr std::size_t initial_budget = 64 * 1024;
	analyzer.SetSpectrumCacheBudget(initial_budget);
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

	auto metrics = analyzer.Metrics();
	EXPECT_EQ(initial_budget, metrics.configured_spectrum_budget_bytes);
	EXPECT_EQ(2, metrics.spectrum_cache_count);
	EXPECT_LE(metrics.spectrum_cache_budget_bytes, initial_budget);
	EXPECT_LE(metrics.spectrum_cache_bytes, metrics.spectrum_cache_budget_bytes);
	EXPECT_EQ(0, metrics.spectrum_cache_hits);
	EXPECT_EQ(2, metrics.spectrum_cache_misses);
	EXPECT_EQ(2, metrics.spectrum_visible_builds);

	constexpr std::size_t reduced_budget = 32 * 1024;
	analyzer.SetSpectrumCacheBudget(reduced_budget);
	metrics = analyzer.Metrics();
	EXPECT_EQ(reduced_budget, metrics.configured_spectrum_budget_bytes);
	EXPECT_EQ(2, metrics.spectrum_cache_count);
	EXPECT_LE(metrics.spectrum_cache_budget_bytes, reduced_budget);
	EXPECT_LE(metrics.spectrum_cache_bytes, metrics.spectrum_cache_budget_bytes);
	EXPECT_EQ(2, metrics.spectrum_cache_misses);
	EXPECT_EQ(2, metrics.spectrum_visible_builds);
}
