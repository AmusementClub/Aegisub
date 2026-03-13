// Copyright (c) 2026

#include <main.h>

#include "../../src/audio_display_analysis.h"
#include "../../src/audio_display_source.h"
#include "../../src/audio_mix_policy.h"

#include <libaegisub/audio/provider.h>

#include <cmath>

namespace {
struct Int16StereoProvider final : agi::AudioProvider {
	Int16StereoProvider() {
		channels = 2;
		num_samples = 4;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const int16_t samples[] = {
			32767, -32768,
			16384, -16384,
			8192, -8192,
			4096, -4096,
		};
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = samples[(start + i) * 2 + 0];
			out[i * 2 + 1] = samples[(start + i) * 2 + 1];
		}
	}
};

struct FloatStereoProvider final : agi::AudioProvider {
	FloatStereoProvider() {
		channels = 2;
		num_samples = 2;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(float);
		float_samples = true;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const float samples[] = {
			0.25f, -0.5f,
			0.75f, -1.0f,
		};
		auto out = static_cast<float *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = samples[(start + i) * 2 + 0];
			out[i * 2 + 1] = samples[(start + i) * 2 + 1];
		}
	}
};
}

TEST(lagi_audio_display, display_source_converts_s16_stereo_to_float) {
	Int16StereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);
	EXPECT_EQ(2, source->GetChannels());
	EXPECT_EQ(48000, source->GetSampleRate());

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(32767.0f / 32768.0f, samples[0], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[1], 1e-6f);
	EXPECT_NEAR(16384.0f / 32768.0f, samples[2], 1e-6f);
	EXPECT_NEAR(-16384.0f / 32768.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, display_source_preserves_float_samples) {
	FloatStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(0.25f, samples[0], 1e-6f);
	EXPECT_NEAR(-0.5f, samples[1], 1e-6f);
	EXPECT_NEAR(0.75f, samples[2], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, mix_policy_average_and_maxabs) {
	const float frame[] = { 0.25f, -0.75f, 0.5f };
	EXPECT_NEAR(0.0f, MixAudioFrameToMono(AudioMixPolicy::MonoAverage, frame, 3), 1e-6f);
	EXPECT_NEAR(-0.75f, MixAudioFrameToMono(AudioMixPolicy::MonoMaxAbs, frame, 3), 1e-6f);
}

TEST(lagi_audio_display, waveform_analysis_uses_requested_mix_policy) {
	const float interleaved[] = {
		0.25f, -0.5f,
		0.5f, -0.25f,
		-0.75f, 0.25f,
		0.1f, -0.9f,
	};

	auto average = AnalyzeWaveformInterleaved(interleaved, 4, 2, AudioMixPolicy::MonoAverage);
	auto maxabs = AnalyzeWaveformInterleaved(interleaved, 4, 2, AudioMixPolicy::MonoMaxAbs);

	EXPECT_LT(average.peak_max, maxabs.peak_max);
	EXPECT_GT(average.peak_min, maxabs.peak_min);
	EXPECT_NEAR(-0.9f, maxabs.peak_min, 1e-6f);
	EXPECT_NEAR(0.5f, maxabs.peak_max, 1e-6f);
}
