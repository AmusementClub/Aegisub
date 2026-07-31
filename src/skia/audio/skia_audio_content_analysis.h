#pragma once

#include "skia_audio_content.h"

#include "../../audio_mix_policy.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class AudioDisplaySource;

namespace aegisub::skia::audio {

enum class ContentBuildStatus {
	Ready,
	Cancelled,
	InvalidRequest,
};

struct ContentBuildResult {
	ContentBuildStatus status = ContentBuildStatus::InvalidRequest;
	std::shared_ptr<ContentTile const> tile;
};

using ContentCancellationCheck = std::function<bool(ContentGeneration)>;

struct WaveformBuildRequest {
	ContentTileKey key;
	double milliseconds_per_pixel = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoMaxAbs;
};

enum class SpectrumChannelMode {
	MixedMono,
	PerBinMaxPower,
	PerBinAveragePower,
};

struct SpectrumBuildRequest {
	ContentTileKey key;
	double milliseconds_per_pixel = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoAverage;
	SpectrumChannelMode channel_mode = SpectrumChannelMode::MixedMono;
	std::size_t derivation_size = 0;
	std::size_t derivation_distance = 0;
};

struct ContentAnalysisCacheMetrics {
	std::size_t configured_spectrum_budget_bytes = 0;
	std::size_t spectrum_cache_count = 0;
	std::size_t spectrum_cache_budget_bytes = 0;
	std::size_t spectrum_cache_bytes = 0;
	std::size_t spectrum_cache_entries = 0;
	std::uint64_t spectrum_cache_hits = 0;
	std::uint64_t spectrum_cache_misses = 0;
	std::uint64_t spectrum_visible_builds = 0;
	std::uint64_t spectrum_cache_evictions = 0;
};

// Owned by one analysis worker for one provider lifetime. FFT resources and a
// small bounded power cache are reused across spectrum tile requests. Destroy
// the analyzer (and join its worker) before the referenced source/provider.
class ContentAnalyzer final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit ContentAnalyzer(AudioDisplaySource& source);
	~ContentAnalyzer();

	ContentAnalyzer(ContentAnalyzer const&) = delete;
	ContentAnalyzer& operator=(ContentAnalyzer const&) = delete;

	void SetSpectrumCacheBudget(std::size_t budget_bytes);
	ContentAnalysisCacheMetrics Metrics() const;

	ContentBuildResult BuildWaveform(
		WaveformBuildRequest const& request,
		ContentCancellationCheck const& is_current = {});
	ContentBuildResult BuildSpectrum(
		SpectrumBuildRequest const& request,
		ContentCancellationCheck const& is_current = {});
};

}
