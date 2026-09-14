#pragma once

#include "audio_mix_policy.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct AudioWaveformSummary {
	float peak_min = 0.f;
	float peak_max = 0.f;
	float avg_min = 0.f;
	float avg_max = 0.f;
};

// Completed audio retains a final partial cache block. In-progress audio only
// exposes complete blocks so that unread samples cannot be cached as silence.
size_t GetAudioDisplayBlockCount(int64_t samples, int sample_rate, double pixel_ms, size_t block_width);
int GetAudioDisplayRenderLength(int start, int length, int64_t samples, int64_t decoded_samples,
								int sample_rate, double pixel_ms, int block_width);
int64_t GetAudioDisplayReadySamples(int64_t samples, int64_t decoded_samples, int64_t lookahead);
// An FFT whose window ends exactly at decoded_samples is ready. Once decoding
// finishes, every in-file FFT center may use the provider's normal EOF padding.
size_t GetAudioSpectrumReadyBlockCount(int64_t samples, int64_t decoded_samples,
									   int64_t half_window, int64_t hop_samples);

AudioWaveformSummary AnalyzeWaveformInterleaved(const float *samples, int frames, int channels, AudioMixPolicy policy);
void MergeSpectrumPowerBinsMax(const std::vector<const float *> &channels, size_t bin_count, float *dst);
void MergeSpectrumPowerBinsAverage(const std::vector<const float *> &channels, size_t bin_count, float *dst);
