#pragma once

enum class AudioMixPolicy {
	MonoAverage,
	MonoMaxAbs,
};

float MixAudioFrameToMono(AudioMixPolicy policy, const float *samples, int channels);
void MixAudioToMono(AudioMixPolicy policy, const float *src, int frames, int channels, float *dst);
