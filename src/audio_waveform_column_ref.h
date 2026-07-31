#pragma once

#include "audio_waveform_summary_cache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

struct AudioWaveformSummaryColumnRef {
	size_t block_index = 0;
	size_t summary_index = 0;
};

struct AudioWaveformPrefetchBlockRange {
	size_t first = 0;
	size_t last = 0;
};

inline bool GetWaveformColumnSampleStart(
	size_t pixel_index,
	double samples_per_pixel,
	int64_t &start) {
	const size_t block_first_pixel = pixel_index
		- pixel_index % AudioWaveformSummaryBlock::width;
	double value = static_cast<double>(block_first_pixel) * samples_per_pixel;
	for (size_t pixel = block_first_pixel; pixel < pixel_index; ++pixel)
		value += samples_per_pixel;

	const double int64_exclusive_upper
		= -static_cast<double>(std::numeric_limits<int64_t>::min());
	if (!std::isfinite(value) || value < 0.0 || value >= int64_exclusive_upper)
		return false;

	start = static_cast<int64_t>(value);
	return true;
}

inline std::optional<int64_t> GetWaveformSummaryBlockSampleEnd(
	size_t block_index,
	double samples_per_pixel) {
	const double int64_exclusive_upper
		= -static_cast<double>(std::numeric_limits<int64_t>::min());
	if (!std::isfinite(samples_per_pixel) || samples_per_pixel <= 0.0
		|| samples_per_pixel >= int64_exclusive_upper
		|| block_index > std::numeric_limits<size_t>::max() / AudioWaveformSummaryBlock::width) {
		return std::nullopt;
	}

	const int64_t samples_per_column = static_cast<int64_t>(samples_per_pixel);
	const size_t block_first_pixel = block_index * AudioWaveformSummaryBlock::width;
	if (block_first_pixel > std::numeric_limits<size_t>::max()
		- (AudioWaveformSummaryBlock::width - 1)) {
		return std::nullopt;
	}

	int64_t last_column_start = 0;
	if (!GetWaveformColumnSampleStart(
		block_first_pixel + AudioWaveformSummaryBlock::width - 1,
		samples_per_pixel,
		last_column_start)
		|| last_column_start > std::numeric_limits<int64_t>::max() - samples_per_column) {
		return std::nullopt;
	}
	return last_column_start + samples_per_column;
}

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

inline std::optional<AudioWaveformPrefetchBlockRange> PlanWaveformPrefetchBlocks(
	int start,
	int length,
	int64_t decoded_samples,
	int sample_rate,
	double pixel_ms,
	size_t margin_blocks = 8) {
	if (start < 0 || length <= 0 || decoded_samples <= 0 || sample_rate <= 0 || pixel_ms <= 0.0)
		return std::nullopt;

	const int64_t last_column = static_cast<int64_t>(start) + length - 1;
	if (last_column < start
		|| static_cast<uint64_t>(last_column) > std::numeric_limits<size_t>::max()) {
		return std::nullopt;
	}
	const size_t first_visible = static_cast<size_t>(start) / AudioWaveformSummaryBlock::width;
	const size_t last_visible = static_cast<size_t>(last_column) / AudioWaveformSummaryBlock::width;
	const size_t first_block = first_visible > margin_blocks ? first_visible - margin_blocks : 0;
	const size_t last_block = last_visible <= std::numeric_limits<size_t>::max() - margin_blocks
		? last_visible + margin_blocks
		: std::numeric_limits<size_t>::max();

	const double samples_per_pixel = pixel_ms * sample_rate / 1000.0;
	auto is_fully_decoded = [&](size_t block_index) {
		auto const sample_end = GetWaveformSummaryBlockSampleEnd(block_index, samples_per_pixel);
		return sample_end && *sample_end <= decoded_samples;
	};
	if (!is_fully_decoded(first_block))
		return std::nullopt;

	size_t last_fully_decoded = last_block;
	if (!is_fully_decoded(last_block)) {
		size_t known_complete = first_block;
		size_t known_incomplete = last_block;
		while (known_complete + 1 < known_incomplete) {
			const size_t candidate = known_complete
				+ (known_incomplete - known_complete) / 2;
			if (is_fully_decoded(candidate))
				known_complete = candidate;
			else
				known_incomplete = candidate;
		}
		last_fully_decoded = known_complete;
	}

	return AudioWaveformPrefetchBlockRange {
		first_block,
		last_fully_decoded
	};
}
