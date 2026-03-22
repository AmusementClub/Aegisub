#pragma once

#include "audio_mix_policy.h"

#include <cstddef>
#include <vector>

struct AudioWaveformSummary {
	float peak_min = 0.f;
	float peak_max = 0.f;
	float avg_min = 0.f;
	float avg_max = 0.f;
};

AudioWaveformSummary AnalyzeWaveformInterleaved(const float *samples, int frames, int channels, AudioMixPolicy policy);
void MergeSpectrumPowerBinsMax(const std::vector<const float *> &channels, size_t bin_count, float *dst);
void MergeSpectrumPowerBinsAverage(const std::vector<const float *> &channels, size_t bin_count, float *dst);
bool ShouldRefreshTrackCursor(int old_pos, int new_pos);
