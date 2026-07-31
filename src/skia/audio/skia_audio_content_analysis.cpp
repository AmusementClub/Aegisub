#include "skia_audio_content_analysis.h"

#include "../../audio_display_source.h"
#include "../../audio_display_analysis.h"
#include "../../audio_spectrum_analysis_cache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace aegisub::skia::audio {
namespace {

constexpr std::size_t kWaveformScratchBudget = 1024 * 1024;
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
	std::size_t spectrum_cache_budget = kMinimumSpectrumAnalysisBudgetBytes;
	std::unique_ptr<AudioSpectrumAnalysisCache> spectrum_cache;
	AudioMixPolicy spectrum_mix_policy = AudioMixPolicy::MonoAverage;
	std::size_t spectrum_derivation_size = 0;
	std::size_t spectrum_derivation_distance = 0;
	SpectrumChannelMode spectrum_channel_mode = SpectrumChannelMode::MixedMono;
	std::vector<std::unique_ptr<AudioDisplaySource>> per_channel_sources;
	std::vector<std::unique_ptr<AudioSpectrumAnalysisCache>> per_channel_caches;

	explicit Impl(AudioDisplaySource& source)
	: source(source) {
	}

	std::size_t PerChannelBudget(std::size_t channel) const noexcept {
		auto const channel_count = std::max<std::size_t>(1, per_channel_caches.size());
		return spectrum_cache_budget / channel_count
			+ (channel < spectrum_cache_budget % channel_count ? 1 : 0);
	}

	void ApplySpectrumCacheBudget() {
		if (spectrum_cache)
			spectrum_cache->Age(spectrum_cache_budget);
		for (std::size_t channel = 0; channel < per_channel_caches.size(); ++channel) {
			if (per_channel_caches[channel])
				per_channel_caches[channel]->Age(std::max<std::size_t>(1, PerChannelBudget(channel)));
		}
	}

	AudioSpectrumAnalysisCache& SpectrumCache(SpectrumBuildRequest const& request) {
		if (!spectrum_cache
			|| spectrum_mix_policy != request.mix_policy
			|| spectrum_derivation_size != request.derivation_size
			|| spectrum_derivation_distance != request.derivation_distance
			|| spectrum_channel_mode != SpectrumChannelMode::MixedMono) {
			spectrum_cache = std::make_unique<AudioSpectrumAnalysisCache>();
			spectrum_mix_policy = request.mix_policy;
			spectrum_derivation_size = request.derivation_size;
			spectrum_derivation_distance = request.derivation_distance;
			spectrum_channel_mode = SpectrumChannelMode::MixedMono;
			spectrum_cache->SetMixPolicy(request.mix_policy);
			spectrum_cache->SetSource(&source);
			spectrum_cache->SetResolution(request.derivation_size, request.derivation_distance);
			spectrum_cache->Age(spectrum_cache_budget);
		}
		return *spectrum_cache;
	}

	void EnsurePerChannelCaches(SpectrumBuildRequest const& request) {
		if (request.channel_mode == SpectrumChannelMode::MixedMono || source.GetChannels() <= 1) {
			per_channel_sources.clear();
			per_channel_caches.clear();
			return;
		}

		if (spectrum_channel_mode != request.channel_mode
			|| spectrum_derivation_size != request.derivation_size
			|| spectrum_derivation_distance != request.derivation_distance
			|| per_channel_caches.size() != static_cast<std::size_t>(source.GetChannels())) {
			per_channel_sources.clear();
			per_channel_caches.clear();
			spectrum_cache.reset();
			spectrum_channel_mode = request.channel_mode;
			spectrum_derivation_size = request.derivation_size;
			spectrum_derivation_distance = request.derivation_distance;
			for (int channel = 0; channel < source.GetChannels(); ++channel) {
				per_channel_sources.push_back(CreateSingleChannelAudioDisplaySource(&source, channel));
				auto cache = std::make_unique<AudioSpectrumAnalysisCache>();
				cache->SetSource(per_channel_sources.back().get());
				cache->SetResolution(request.derivation_size, request.derivation_distance);
				per_channel_caches.push_back(std::move(cache));
			}
			ApplySpectrumCacheBudget();
		}
	}
};

ContentAnalyzer::ContentAnalyzer(AudioDisplaySource& source)
: impl(std::make_unique<Impl>(source)) {
}

ContentAnalyzer::~ContentAnalyzer() = default;

void ContentAnalyzer::SetSpectrumCacheBudget(std::size_t budget_bytes) {
	budget_bytes = std::max<std::size_t>(1, budget_bytes);
	if (impl->spectrum_cache_budget == budget_bytes)
		return;
	impl->spectrum_cache_budget = budget_bytes;
	impl->ApplySpectrumCacheBudget();
}

ContentAnalysisCacheMetrics ContentAnalyzer::Metrics() const {
	ContentAnalysisCacheMetrics metrics;
	metrics.configured_spectrum_budget_bytes = impl->spectrum_cache_budget;
	auto append = [&metrics](AudioSpectrumAnalysisCache const& cache) {
		auto const snapshot = cache.GetMetricsSnapshot();
		++metrics.spectrum_cache_count;
		metrics.spectrum_cache_budget_bytes += snapshot.cache_budget_bytes;
		metrics.spectrum_cache_bytes += snapshot.cache_bytes;
		metrics.spectrum_cache_entries += snapshot.cache_entries;
		metrics.spectrum_cache_hits += snapshot.cache_hits;
		metrics.spectrum_cache_misses += snapshot.cache_misses;
		metrics.spectrum_visible_builds += snapshot.visible_builds;
		metrics.spectrum_cache_evictions += snapshot.evictions;
	};
	if (impl->spectrum_cache)
		append(*impl->spectrum_cache);
	for (auto const& cache : impl->per_channel_caches) {
		if (cache)
			append(*cache);
	}
	return metrics;
}

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
		|| request.key.spectrum_bin_count != (static_cast<std::size_t>(1) << request.derivation_size)
		|| (request.channel_mode != SpectrumChannelMode::MixedMono
			&& source.GetChannels() <= 1)) {
		return {};
	}
	if (!Current(request.key.generation, is_current))
		return { ContentBuildStatus::Cancelled, {} };

	auto const samples_per_pixel = static_cast<long double>(request.milliseconds_per_pixel)
		* source.GetSampleRate() / 1000.0L;
	if (!std::isfinite(samples_per_pixel) || samples_per_pixel <= 0.0L)
		return {};

	auto const bin_count = static_cast<std::size_t>(request.key.spectrum_bin_count);
	impl->EnsurePerChannelCaches(request);
	AudioSpectrumAnalysisCache *cache = nullptr;
	if (request.channel_mode == SpectrumChannelMode::MixedMono || source.GetChannels() <= 1) {
		cache = &impl->SpectrumCache(request);
		if (!cache->IsReady())
			return {};
	}
	else {
		if (impl->per_channel_caches.empty())
			return {};
		for (auto const& channel_cache : impl->per_channel_caches) {
			if (!channel_cache || !channel_cache->IsReady())
				return {};
		}
	}

	auto tile = std::make_shared<ContentTile>();
	tile->key = request.key;
	tile->spectrum_power.resize(static_cast<std::size_t>(request.key.column_count) * bin_count);
	std::size_t previous_block = std::numeric_limits<std::size_t>::max();
	AudioSpectrumAnalysisCache::BlockHandle previous_power;
	std::vector<float> merged_power;
	std::vector<const float *> channel_power_inputs;
	std::vector<AudioSpectrumAnalysisCache::BlockHandle> channel_power_blocks;
	if (!cache) {
		merged_power.resize(bin_count);
		channel_power_inputs.resize(impl->per_channel_caches.size());
		channel_power_blocks.resize(impl->per_channel_caches.size());
	}
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
			if (cache) {
				previous_power = cache->Get(block_index);
			}
			else {
				for (std::size_t channel = 0; channel < impl->per_channel_caches.size(); ++channel) {
					channel_power_blocks[channel] = impl->per_channel_caches[channel]->Get(block_index);
					channel_power_inputs[channel] = channel_power_blocks[channel].get();
				}
				if (request.channel_mode == SpectrumChannelMode::PerBinMaxPower)
					MergeSpectrumPowerBinsMax(channel_power_inputs, bin_count, merged_power.data());
				else
					MergeSpectrumPowerBinsAverage(channel_power_inputs, bin_count, merged_power.data());
			}
			previous_block = block_index;
		}
		if (cache && !previous_power)
			return {};
		if (!cache && merged_power.empty())
			return {};
		auto const *power = cache ? previous_power.get() : merged_power.data();
		std::copy(power, power + bin_count,
			tile->spectrum_power.begin() + static_cast<std::size_t>(column) * bin_count);
	}
	if (!tile->IsValid())
		return {};
	return { ContentBuildStatus::Ready, std::move(tile) };
}

}
