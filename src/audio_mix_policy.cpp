#include "audio_mix_policy.h"

#include <cmath>

float MixAudioFrameToMono(AudioMixPolicy policy, const float *samples, int channels) {
	if (!samples || channels <= 0)
		return 0.f;

	if (channels == 1)
		return samples[0];

	switch (policy) {
		case AudioMixPolicy::MonoMaxAbs: {
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
		case AudioMixPolicy::MonoAverage:
		default: {
			double sum = 0.0;
			for (int i = 0; i < channels; ++i)
				sum += samples[i];
			return static_cast<float>(sum / channels);
		}
	}
}

void MixAudioToMono(AudioMixPolicy policy, const float *src, int frames, int channels, float *dst) {
	if (!src || !dst || frames <= 0 || channels <= 0)
		return;

	for (int i = 0; i < frames; ++i)
		dst[i] = MixAudioFrameToMono(policy, src + i * channels, channels);
}
