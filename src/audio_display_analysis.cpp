#include "audio_display_analysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

size_t GetAudioDisplayBlockCount(int64_t samples, int sample_rate, double pixel_ms, size_t block_width) {
	if (samples <= 0 || sample_rate <= 0 || pixel_ms <= 0.0 || block_width == 0)
		return 0;

	const double blocks = std::ceil(samples * 1000.0 / sample_rate / pixel_ms / block_width);
	const double size_t_exclusive_upper = std::ldexp(1.0, std::numeric_limits<size_t>::digits);
	if (!std::isfinite(blocks) || blocks <= 0.0 || blocks >= size_t_exclusive_upper)
		return 0;
	return static_cast<size_t>(blocks);
}

int GetAudioDisplayRenderLength(int start, int length, int64_t samples, int64_t decoded_samples,
								int sample_rate, double pixel_ms, int block_width) {
	if (start < 0 || length <= 0 || decoded_samples <= 0 || block_width <= 0 || GetAudioDisplayBlockCount(samples, sample_rate, pixel_ms, block_width) == 0) {
		return 0;
	}

	const double available_pixels = std::min(samples, decoded_samples) * 1000.0 / sample_rate / pixel_ms;
	const double end_pixel = decoded_samples >= samples
								 ? std::ceil(available_pixels)
								 : std::floor(available_pixels / block_width) * block_width;
	return static_cast<int>(std::clamp(end_pixel - start, 0.0, static_cast<double>(length)));
}

int64_t GetAudioDisplayReadySamples(int64_t samples, int64_t decoded_samples, int64_t lookahead) {
	if (decoded_samples >= samples)
		return std::max<int64_t>(0, samples);
	return decoded_samples > lookahead ? decoded_samples - lookahead : 0;
}

size_t GetAudioSpectrumReadyBlockCount(int64_t samples, int64_t decoded_samples,
									   int64_t half_window, int64_t hop_samples) {
	if (samples <= 0 || hop_samples <= 0)
		return 0;
	const int64_t total_blocks = samples / hop_samples + (samples % hop_samples != 0);
	if (decoded_samples >= samples)
		return static_cast<size_t>(total_blocks);
	if (decoded_samples < half_window)
		return 0;
	return static_cast<size_t>(std::min(total_blocks, (decoded_samples - half_window) / hop_samples + 1));
}

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

