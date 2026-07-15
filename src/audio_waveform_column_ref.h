#pragma once

#include "audio_waveform_summary_cache.h"

#include <cstddef>
#include <vector>

struct AudioWaveformSummaryColumnRef {
	size_t block_index = 0;
	size_t summary_index = 0;
};

inline AudioWaveformSummaryColumnRef GetWaveformSummaryColumnRef(int pixel_index) {
	auto const absolute_pixel = pixel_index < 0 ? 0 : pixel_index;
	return {
		static_cast<size_t>(absolute_pixel / static_cast<int>(AudioWaveformSummaryBlock::width)),
		static_cast<size_t>(absolute_pixel % static_cast<int>(AudioWaveformSummaryBlock::width))
	};
}

inline std::vector<AudioWaveformSummaryColumnRef> BuildWaveformSummaryColumnRefs(int start, int width) {
	std::vector<AudioWaveformSummaryColumnRef> refs;
	if (width <= 0)
		return refs;

	refs.reserve(static_cast<size_t>(width));
	for (int x = 0; x < width; ++x)
		refs.emplace_back(GetWaveformSummaryColumnRef(start + x));
	return refs;
}
