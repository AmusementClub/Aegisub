#include "audio_mix_policy.h"

#include <cmath>

namespace {
inline float MixAverage(const float *samples, int channels) {
	switch (channels) {
		case 1:
			return samples[0];
		case 2:
			return (samples[0] + samples[1]) * 0.5f;
		case 6:
			return (samples[0] + samples[1] + samples[2] + samples[3] + samples[4] + samples[5]) / 6.0f;
		case 8:
			return (samples[0] + samples[1] + samples[2] + samples[3] + samples[4] + samples[5] + samples[6] + samples[7]) / 8.0f;
		default: {
			double sum = 0.0;
			for (int i = 0; i < channels; ++i)
				sum += samples[i];
			return static_cast<float>(sum / channels);
		}
	}
}

inline float MixMaxAbs(const float *samples, int channels) {
	switch (channels) {
		case 1:
			return samples[0];
		case 2:
			return std::fabs(samples[0]) >= std::fabs(samples[1]) ? samples[0] : samples[1];
		case 6: {
			float best = samples[0];
			float best_abs = std::fabs(best);
			for (int i = 1; i < 6; ++i) {
				float cur = samples[i];
				float cur_abs = std::fabs(cur);
				if (cur_abs > best_abs) {
					best = cur;
					best_abs = cur_abs;
				}
			}
			return best;
		}
		case 8: {
			float best = samples[0];
			float best_abs = std::fabs(best);
			for (int i = 1; i < 8; ++i) {
				float cur = samples[i];
				float cur_abs = std::fabs(cur);
				if (cur_abs > best_abs) {
					best = cur;
					best_abs = cur_abs;
				}
			}
			return best;
		}
	}

	float best = samples[0];
	float best_abs = std::fabs(best);
	for (int i = 1; i < channels; ++i) {
		float cur = samples[i];
		float cur_abs = std::fabs(cur);
		if (cur_abs > best_abs) {
			best = cur;
			best_abs = cur_abs;
		}
	}
	return best;
}
}

float MixAudioFrameToMono(AudioMixPolicy policy, const float *samples, int channels) {
	if (!samples || channels <= 0)
		return 0.f;

	switch (policy) {
		case AudioMixPolicy::MonoMaxAbs:
			return MixMaxAbs(samples, channels);
		case AudioMixPolicy::MonoAverage:
		default:
			return MixAverage(samples, channels);
	}
}

void MixAudioToMono(AudioMixPolicy policy, const float *src, int frames, int channels, float *dst) {
	if (!src || !dst || frames <= 0 || channels <= 0)
		return;

	const float *cur = src;
	if (policy == AudioMixPolicy::MonoAverage) {
		switch (channels) {
			case 1:
				for (int i = 0; i < frames; ++i) dst[i] = cur[i];
				return;
			case 2:
				for (int i = 0; i < frames; ++i, cur += 2) dst[i] = (cur[0] + cur[1]) * 0.5f;
				return;
			case 6:
				for (int i = 0; i < frames; ++i, cur += 6) dst[i] = (cur[0] + cur[1] + cur[2] + cur[3] + cur[4] + cur[5]) / 6.0f;
				return;
			case 8:
				for (int i = 0; i < frames; ++i, cur += 8) dst[i] = (cur[0] + cur[1] + cur[2] + cur[3] + cur[4] + cur[5] + cur[6] + cur[7]) / 8.0f;
				return;
		}
	}
	else if (policy == AudioMixPolicy::MonoMaxAbs) {
		switch (channels) {
			case 1:
				for (int i = 0; i < frames; ++i) dst[i] = cur[i];
				return;
			case 2:
				for (int i = 0; i < frames; ++i, cur += 2) dst[i] = std::fabs(cur[0]) >= std::fabs(cur[1]) ? cur[0] : cur[1];
				return;
		}
	}

	for (int i = 0; i < frames; ++i, cur += channels)
		dst[i] = MixAudioFrameToMono(policy, cur, channels);
}
