#include "audio_display_analysis.h"

AudioWaveformSummary AnalyzeWaveformInterleaved(const float *samples, int frames, int channels, AudioMixPolicy policy) {
	AudioWaveformSummary summary;
	if (!samples || frames <= 0 || channels <= 0)
		return summary;

	float peak_min = 0.f;
	float peak_max = 0.f;
	double avg_min_accum = 0.0;
	double avg_max_accum = 0.0;

	for (int i = 0; i < frames; ++i) {
		float mixed = MixAudioFrameToMono(policy, samples + i * channels, channels);
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
