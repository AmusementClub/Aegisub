#include "audio_display_analysis.h"

#include <algorithm>
#include <cmath>

AudioWaveformSummary AnalyzeWaveformInterleaved(const float *samples, int frames, int channels, AudioMixPolicy policy) {
	AudioWaveformSummary summary;
	if (!samples || frames <= 0 || channels <= 0)
		return summary;

	float peak_min = 0.f;
	float peak_max = 0.f;
	double avg_min_accum = 0.0;
	double avg_max_accum = 0.0;

	if (channels == 1) {
		for (int i = 0; i < frames; ++i) {
			float mixed = samples[i];
			if (mixed > 0.f) {
				if (mixed > peak_max)
					peak_max = mixed;
				avg_max_accum += mixed;
			}
			else {
				if (mixed < peak_min)
					peak_min = mixed;
				avg_min_accum += mixed;
			}
		}

		summary.peak_min = peak_min;
		summary.peak_max = peak_max;
		summary.avg_min = static_cast<float>(avg_min_accum / frames);
		summary.avg_max = static_cast<float>(avg_max_accum / frames);
		return summary;
	}

	if (channels == 2) {
		if (policy == AudioMixPolicy::MonoAverage) {
			for (int i = 0; i < frames; ++i) {
				float mixed = (samples[i * 2] + samples[i * 2 + 1]) * 0.5f;
				if (mixed > 0.f) {
					if (mixed > peak_max)
						peak_max = mixed;
					avg_max_accum += mixed;
				}
				else {
					if (mixed < peak_min)
						peak_min = mixed;
					avg_min_accum += mixed;
				}
			}

			summary.peak_min = peak_min;
			summary.peak_max = peak_max;
			summary.avg_min = static_cast<float>(avg_min_accum / frames);
			summary.avg_max = static_cast<float>(avg_max_accum / frames);
			return summary;
		}
		if (policy == AudioMixPolicy::MonoMaxAbs) {
			for (int i = 0; i < frames; ++i) {
				const float a = samples[i * 2];
				const float b = samples[i * 2 + 1];
				float mixed = std::fabs(a) >= std::fabs(b) ? a : b;
				if (mixed > 0.f) {
					if (mixed > peak_max)
						peak_max = mixed;
					avg_max_accum += mixed;
				}
				else {
					if (mixed < peak_min)
						peak_min = mixed;
					avg_min_accum += mixed;
				}
			}

			summary.peak_min = peak_min;
			summary.peak_max = peak_max;
			summary.avg_min = static_cast<float>(avg_min_accum / frames);
			summary.avg_max = static_cast<float>(avg_max_accum / frames);
			return summary;
		}
	}

	const float *cur = samples;
	for (int i = 0; i < frames; ++i, cur += channels) {
		float mixed = MixAudioFrameToMono(policy, cur, channels);
		if (mixed > 0.f) {
			if (mixed > peak_max)
				peak_max = mixed;
			avg_max_accum += mixed;
		}
		else {
			if (mixed < peak_min)
				peak_min = mixed;
			avg_min_accum += mixed;
		}
	}

	summary.peak_min = peak_min;
	summary.peak_max = peak_max;
	summary.avg_min = static_cast<float>(avg_min_accum / frames);
	summary.avg_max = static_cast<float>(avg_max_accum / frames);
	return summary;
}

void MergeSpectrumPowerBinsMax(const std::vector<const float *> &channels, size_t bin_count, float *dst) {
	if (!dst || bin_count == 0)
		return;

	const float *seed = nullptr;
	for (const float *channel : channels) {
		if (channel) {
			seed = channel;
			break;
		}
	}

	if (!seed) {
		std::fill(dst, dst + bin_count, 0.f);
		return;
	}

	std::copy(seed, seed + bin_count, dst);
	for (const float *channel : channels) {
		if (!channel || channel == seed)
			continue;
		for (size_t i = 0; i < bin_count; ++i)
			dst[i] = std::max(dst[i], channel[i]);
	}
}

void MergeSpectrumPowerBinsAverage(const std::vector<const float *> &channels, size_t bin_count, float *dst) {
	if (!dst || bin_count == 0)
		return;

	std::fill(dst, dst + bin_count, 0.f);
	size_t valid_channels = 0;
	for (const float *channel : channels) {
		if (!channel)
			continue;
		++valid_channels;
		for (size_t i = 0; i < bin_count; ++i)
			dst[i] += channel[i];
	}

	if (valid_channels == 0)
		return;

	const float scale = 1.0f / static_cast<float>(valid_channels);
	for (size_t i = 0; i < bin_count; ++i)
		dst[i] *= scale;
}

