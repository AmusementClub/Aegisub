#include "skia_audio_content_analysis.h"

#include "../../audio_display_source.h"
#include "../../audio_spectrum_analysis_cache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace aegisub::skia::audio {
namespace {

constexpr std::size_t kWaveformScratchBudget = 1024 * 1024;
constexpr std::size_t kSpectrumAnalysisCacheBudget = 8 * 1024 * 1024;
constexpr std::uint32_t kMaximumTileColumns = 512;
constexpr std::uint32_t kMaximumSpectrumBins = 4096;
constexpr std::size_t kMinimumSpectrumDerivationSize = 4;

bool Current(ContentGeneration generation, ContentCancellationCheck const& is_current) {
	return !is_current || is_current(generation);
}

bool ValidCommonRequest(
	AudioDisplaySource const& source,
	ContentTileKey const& key,
	double milliseconds_per_pixel,
	ContentKind expected_kind) {
	if (key.kind != expected_kind
		|| !key.generation.provider
		|| !key.generation.analysis
		|| key.column_count == 0
		|| key.column_count > kMaximumTileColumns
		|| !std::isfinite(milliseconds_per_pixel)
		|| milliseconds_per_pixel <= 0.0
		|| source.GetSampleRate() <= 0
		|| source.GetChannels() <= 0
		|| source.GetChannels() > 256
		|| source.GetNumSamples() <= 0) {
		return false;
	}
	return key.tile_index <= std::numeric_limits<std::uint64_t>::max() / key.column_count;
}

bool ColumnStart(
	ContentTileKey const& key,
	std::uint32_t column,
	long double samples_per_pixel,
	std::int64_t& start) {
	auto const first_column = key.tile_index * key.column_count;
	if (first_column > std::numeric_limits<std::uint64_t>::max() - column)
		return false;
	auto const absolute_column = first_column + column;
	auto const value = static_cast<long double>(absolute_column) * samples_per_pixel;
	if (!std::isfinite(value)
		|| value < 0.0L
		|| value > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
		return false;
	}
	start = static_cast<std::int64_t>(value);
	return true;
}

struct WaveformAccumulator {
	float peak_min = 0.f;
	float peak_max = 0.f;
	double average_min_sum = 0.0;
	double average_max_sum = 0.0;
	std::int64_t frames = 0;

	void Add(float const *samples, std::int64_t frame_count, int channels, AudioMixPolicy policy) {
		for (std::int64_t frame = 0; frame < frame_count; ++frame) {
			auto const mixed = MixAudioFrameToMono(
				policy,
				samples + static_cast<std::size_t>(frame) * channels,
				channels);
			if (mixed > 0.f) {
				peak_max = std::max(peak_max, mixed);
				average_max_sum += mixed;
			}
			else {
				peak_min = std::min(peak_min, mixed);
				average_min_sum += mixed;
			}
		}
		frames += frame_count;
	}

	WaveformColumn Finish() const noexcept {
		if (frames <= 0)
			return {};
		auto const divisor = static_cast<double>(frames);
		return {
			peak_min,
			peak_max,
			static_cast<float>(average_min_sum / divisor),
			static_cast<float>(average_max_sum / divisor),
		};
	}
};

}

struct ContentAnalyzer::Impl {
	AudioDisplaySource& source;
	std::unique_ptr<AudioSpectrumAnalysisCache> spectrum_cache;
	AudioMixPolicy spectrum_mix_policy = AudioMixPolicy::MonoAverage;
	std::size_t spectrum_derivation_size = 0;
	std::size_t spectrum_derivation_distance = 0;

	explicit Impl(AudioDisplaySource& source)
	: source(source) {
	}

	AudioSpectrumAnalysisCache& SpectrumCache(SpectrumBuildRequest const& request) {
		if (!spectrum_cache
			|| spectrum_mix_policy != request.mix_policy
			|| spectrum_derivation_size != request.derivation_size
			|| spectrum_derivation_distance != request.derivation_distance) {
			spectrum_cache = std::make_unique<AudioSpectrumAnalysisCache>();
			spectrum_mix_policy = request.mix_policy;
			spectrum_derivation_size = request.derivation_size;
			spectrum_derivation_distance = request.derivation_distance;
			spectrum_cache->SetMixPolicy(request.mix_policy);
			spectrum_cache->SetSource(&source);
			spectrum_cache->SetResolution(request.derivation_size, request.derivation_distance);
			spectrum_cache->Age(kSpectrumAnalysisCacheBudget);
		}
		return *spectrum_cache;
	}
};

ContentAnalyzer::ContentAnalyzer(AudioDisplaySource& source)
: impl(std::make_unique<Impl>(source)) {
}

ContentAnalyzer::~ContentAnalyzer() = default;

ContentBuildResult ContentAnalyzer::BuildWaveform(
	WaveformBuildRequest const& request,
	ContentCancellationCheck const& is_current) {
	auto& source = impl->source;
	if (!ValidCommonRequest(
		source,
		request.key,
		request.milliseconds_per_pixel,
		ContentKind::Waveform)
		|| request.key.spectrum_bin_count != 0) {
		return {};
	}
	if (!Current(request.key.generation, is_current))
		return { ContentBuildStatus::Cancelled, {} };

	auto const samples_per_pixel_exact = static_cast<long double>(request.milliseconds_per_pixel)
		* source.GetSampleRate() / 1000.0L;
	if (!std::isfinite(samples_per_pixel_exact)
		|| samples_per_pixel_exact > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
		return {};
	}
	auto const samples_per_pixel = std::max<std::int64_t>(
		1,
		static_cast<std::int64_t>(samples_per_pixel_exact));
	auto const channels = source.GetChannels();
	auto const scratch_frames = std::max<std::int64_t>(
		1,
		static_cast<std::int64_t>(kWaveformScratchBudget / (static_cast<std::size_t>(channels) * sizeof(float))));
	std::vector<float> scratch(static_cast<std::size_t>(std::min(samples_per_pixel, scratch_frames)) * channels);

	auto tile = std::make_shared<ContentTile>();
	tile->key = request.key;
	tile->waveform.reserve(request.key.column_count);
	for (std::uint32_t column = 0; column < request.key.column_count; ++column) {
		if (!Current(request.key.generation, is_current))
			return { ContentBuildStatus::Cancelled, {} };
		std::int64_t column_start = 0;
		if (!ColumnStart(request.key, column, samples_per_pixel_exact, column_start))
			return {};
		if (column_start > std::numeric_limits<std::int64_t>::max() - samples_per_pixel)
			return {};

		WaveformAccumulator accumulator;
		std::int64_t consumed = 0;
		while (consumed < samples_per_pixel) {
			if (!Current(request.key.generation, is_current))
				return { ContentBuildStatus::Cancelled, {} };
			auto const count = std::min(samples_per_pixel - consumed, scratch_frames);
			source.GetFloatAudio(scratch.data(), column_start + consumed, count);
			accumulator.Add(scratch.data(), count, channels, request.mix_policy);
			consumed += count;
		}
		tile->waveform.push_back(accumulator.Finish());
	}
	if (!tile->IsValid())
		return {};
	return { ContentBuildStatus::Ready, std::move(tile) };
}

ContentBuildResult ContentAnalyzer::BuildSpectrum(
	SpectrumBuildRequest const& request,
	ContentCancellationCheck const& is_current) {
	auto& source = impl->source;
	if (!ValidCommonRequest(
		source,
		request.key,
		request.milliseconds_per_pixel,
		ContentKind::Spectrum)
		|| request.derivation_size < kMinimumSpectrumDerivationSize
		|| request.derivation_size >= std::numeric_limits<std::size_t>::digits
		|| request.derivation_distance > request.derivation_size
		|| request.key.spectrum_bin_count > kMaximumSpectrumBins
		|| request.key.spectrum_bin_count != (static_cast<std::size_t>(1) << request.derivation_size)) {
		return {};
	}
	if (!Current(request.key.generation, is_current))
		return { ContentBuildStatus::Cancelled, {} };

	auto const samples_per_pixel = static_cast<long double>(request.milliseconds_per_pixel)
		* source.GetSampleRate() / 1000.0L;
	if (!std::isfinite(samples_per_pixel) || samples_per_pixel <= 0.0L)
		return {};

	auto& cache = impl->SpectrumCache(request);
	if (!cache.IsReady())
		return {};
	auto const bin_count = static_cast<std::size_t>(request.key.spectrum_bin_count);

	auto tile = std::make_shared<ContentTile>();
	tile->key = request.key;
	tile->spectrum_power.resize(static_cast<std::size_t>(request.key.column_count) * bin_count);
	std::size_t previous_block = std::numeric_limits<std::size_t>::max();
	float const *previous_power = nullptr;
	for (std::uint32_t column = 0; column < request.key.column_count; ++column) {
		if (!Current(request.key.generation, is_current))
			return { ContentBuildStatus::Cancelled, {} };
		std::int64_t column_start = 0;
		if (!ColumnStart(request.key, column, samples_per_pixel, column_start))
			return {};
		auto const block_index_u64 = static_cast<std::uint64_t>(column_start) >> request.derivation_distance;
		if (block_index_u64 > std::numeric_limits<std::size_t>::max())
			return {};
		auto const block_index = static_cast<std::size_t>(block_index_u64);
		if (block_index != previous_block) {
			previous_power = cache.Get(block_index);
			previous_block = block_index;
		}
		if (!previous_power)
			return {};
		std::copy(
			previous_power,
			previous_power + bin_count,
			tile->spectrum_power.begin() + static_cast<std::size_t>(column) * bin_count);
	}
	if (!tile->IsValid())
		return {};
	return { ContentBuildStatus::Ready, std::move(tile) };
}

}
