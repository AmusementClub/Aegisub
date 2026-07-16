#pragma once

#include "skia_audio_content.h"

#include "../../audio_mix_policy.h"

#include <cstddef>
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

struct SpectrumBuildRequest {
	ContentTileKey key;
	double milliseconds_per_pixel = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoAverage;
	std::size_t derivation_size = 0;
	std::size_t derivation_distance = 0;
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

	ContentBuildResult BuildWaveform(
		WaveformBuildRequest const& request,
		ContentCancellationCheck const& is_current = {});
	ContentBuildResult BuildSpectrum(
		SpectrumBuildRequest const& request,
		ContentCancellationCheck const& is_current = {});
};

}
