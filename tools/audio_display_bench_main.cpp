#include "audio_display_analysis.h"
#include "audio_display_source.h"
#include "audio_mix_policy.h"
#include "audio_renderer.h"
#include "audio_spectrum_analysis_cache.h"
#include "audio_waveform_summary_cache.h"
#include "fft.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/init.h>

namespace {
using clock_type = std::chrono::steady_clock;

struct BenchResult {
	std::string name;
	int iterations = 0;
	double total_ms = 0.0;
	double avg_ms = 0.0;
	double throughput_mframes_s = 0.0;
};

struct OldWaveformSummary {
	int peak_min = 0;
	int peak_max = 0;
	int avg_min = 0;
	int avg_max = 0;
};

struct SyntheticInt16StereoProvider final : agi::AudioProvider {
	std::vector<int16_t> data;

	SyntheticInt16StereoProvider(int64_t frames) {
		channels = 2;
		num_samples = frames;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
		data.resize(static_cast<size_t>(frames) * channels);
		for (int64_t i = 0; i < frames; ++i) {
			data[static_cast<size_t>(i) * 2 + 0] = static_cast<int16_t>((i * 37) % 32767);
			data[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(-((i * 53) % 32768));
		}
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		std::memcpy(buf, data.data() + start * channels, static_cast<size_t>(count) * channels * sizeof(int16_t));
	}
};

struct SyntheticInt16InterleavedProvider final : agi::AudioProvider {
	std::vector<int16_t> data;

	SyntheticInt16InterleavedProvider(int64_t frames, int channel_count) {
		channels = channel_count;
		num_samples = frames;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
		data.resize(static_cast<size_t>(frames) * channels);
		for (int64_t i = 0; i < frames; ++i) {
			for (int ch = 0; ch < channels; ++ch) {
				int value = static_cast<int>(((i * (17 + ch * 7)) + ch * 113) % 65536) - 32768;
				data[static_cast<size_t>(i) * channels + ch] = static_cast<int16_t>(value);
			}
		}
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		std::memcpy(buf, data.data() + start * channels, static_cast<size_t>(count) * channels * sizeof(int16_t));
	}
};

struct SyntheticSilentProvider final : agi::AudioProvider {
	SyntheticSilentProvider(int64_t frames, int channel_count) {
		channels = channel_count;
		num_samples = frames;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t, int64_t count) const override {
		std::memset(buf, 0, static_cast<size_t>(count) * channels * bytes_per_sample);
	}
};

struct NoOpAudioBitmapProvider final : AudioRendererBitmapProvider {
	void Render(wxBitmap &, int, AudioRenderingStyle) override {
	}

	void RenderBlank(wxDC &, const wxRect &, AudioRenderingStyle) override {
	}
};

BenchResult RunAudioRendererTileDrawHotBench(int cache_bitmap_width, const char *name) {
	constexpr int iterations = 3000;
	constexpr int viewport_width = 2048;
	constexpr int viewport_height = 256;
	constexpr int64_t frames = 48000 * 60 * 10;

	SyntheticSilentProvider provider(frames, 2);
	NoOpAudioBitmapProvider bitmap_provider;
	(void)cache_bitmap_width;
	AudioRenderer renderer;
	renderer.SetCacheMaxSize(256ull * 1024 * 1024);
	renderer.SetHeight(viewport_height);
	renderer.SetAudioProvider(&provider);
	renderer.SetRenderer(&bitmap_provider);

	wxBitmap canvas(viewport_width, viewport_height);
	wxMemoryDC dc(canvas);

	renderer.Render(dc, wxPoint(0, 0), 0, viewport_width, AudioStyle_Normal);

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		renderer.Render(dc, wxPoint(0, 0), 0, viewport_width, AudioStyle_Normal);
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { name, iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunAudioRendererTileDrawColdBench(int cache_bitmap_width, const char *name) {
	constexpr int iterations = 100;
	constexpr int viewport_width = 2048;
	constexpr int viewport_height = 256;
	constexpr int64_t frames = 48000 * 60 * 10;

	SyntheticSilentProvider provider(frames, 2);
	NoOpAudioBitmapProvider bitmap_provider;
	(void)cache_bitmap_width;
	AudioRenderer renderer;
	renderer.SetCacheMaxSize(256ull * 1024 * 1024);
	renderer.SetHeight(viewport_height);
	renderer.SetAudioProvider(&provider);
	renderer.SetRenderer(&bitmap_provider);

	wxBitmap canvas(viewport_width, viewport_height);
	wxMemoryDC dc(canvas);

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		renderer.Invalidate();
		renderer.Render(dc, wxPoint(0, 0), 0, viewport_width, AudioStyle_Normal);
	}
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { name, iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunDisplaySourceBench() {
	constexpr int64_t frames = 1 << 18;
	constexpr int iterations = 50;
	SyntheticInt16StereoProvider provider(frames);
	auto source = CreateAudioDisplaySource(&provider);
	std::vector<float> out(static_cast<size_t>(frames) * provider.GetChannels());

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		source->GetFloatAudio(out.data(), 0, frames);
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "display_source_s16_stereo", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunLegacySingleChannelDisplaySourceBench() {
	constexpr int64_t frames = 1 << 18;
	constexpr int channels = 8;
	constexpr int channel = 5;
	constexpr int iterations = 40;
	SyntheticInt16InterleavedProvider provider(frames, channels);
	auto source = CreateAudioDisplaySource(&provider);
	std::vector<float> interleaved(static_cast<size_t>(frames) * channels);
	std::vector<float> out(static_cast<size_t>(frames));

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		source->GetFloatAudio(interleaved.data(), 0, frames);
		const float *src = interleaved.data() + channel;
		for (int64_t frame = 0; frame < frames; ++frame, src += channels)
			out[static_cast<size_t>(frame)] = *src;
	}
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "legacy_single_channel_s16_8ch", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunDirectSingleChannelDisplaySourceBench() {
	constexpr int64_t frames = 1 << 18;
	constexpr int channels = 8;
	constexpr int channel = 5;
	constexpr int iterations = 40;
	SyntheticInt16InterleavedProvider provider(frames, channels);
	auto source = CreateAudioDisplaySource(&provider);
	auto single = CreateSingleChannelAudioDisplaySource(source.get(), channel);
	std::vector<float> out(static_cast<size_t>(frames));

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		single->GetFloatAudio(out.data(), 0, frames);
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "direct_single_channel_s16_8ch", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunOldMonoFetchBench() {
	constexpr int64_t frames = 1 << 18;
	constexpr int iterations = 50;
	SyntheticInt16StereoProvider provider(frames);
	std::vector<int16_t> out(static_cast<size_t>(frames));

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		provider.GetInt16MonoAudio(out.data(), 0, frames);
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "old_mono_fetch_s16", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunMixBench(AudioMixPolicy policy, const char *name) {
	constexpr int frames = 1 << 18;
	constexpr int channels = 6;
	constexpr int iterations = 100;
	std::vector<float> src(static_cast<size_t>(frames) * channels);
	std::vector<float> dst(frames);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = static_cast<float>((static_cast<int>(i % 1024) - 512) / 512.0);

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		MixAudioToMono(policy, src.data(), frames, channels, dst.data());
	auto t1 = clock_type::now();

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { name, iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunWaveformBench() {
	constexpr int frames = 1 << 16;
	constexpr int channels = 6;
	constexpr int iterations = 200;
	std::vector<float> src(static_cast<size_t>(frames) * channels);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = static_cast<float>((static_cast<int>(i % 2048) - 1024) / 1024.0);

	volatile float sink = 0.f;
	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		auto summary = AnalyzeWaveformInterleaved(src.data(), frames, channels, AudioMixPolicy::MonoMaxAbs);
		sink += summary.peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "waveform_analysis_maxabs", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

OldWaveformSummary AnalyzeOldWaveformMonoInt16(const int16_t *samples, int frames) {
	OldWaveformSummary summary;
	if (!samples || frames <= 0)
		return summary;

	int peak_min = 0, peak_max = 0;
	int64_t avg_min_accum = 0, avg_max_accum = 0;
	for (int i = 0; i < frames; ++i) {
		int16_t sample = samples[i];
		if (sample > 0) {
			peak_max = std::max(peak_max, static_cast<int>(sample));
			avg_max_accum += sample;
		}
		else {
			peak_min = std::min(peak_min, static_cast<int>(sample));
			avg_min_accum += sample;
		}
	}

	summary.peak_min = peak_min;
	summary.peak_max = peak_max;
	summary.avg_min = static_cast<int>(avg_min_accum / frames);
	summary.avg_max = static_cast<int>(avg_max_accum / frames);
	return summary;
}

BenchResult RunOldWaveformBench() {
	constexpr int frames = 1 << 16;
	constexpr int iterations = 200;
	SyntheticInt16StereoProvider provider(frames);
	std::vector<int16_t> mono(frames);
	volatile int sink = 0;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		provider.GetInt16MonoAudio(mono.data(), 0, frames);
		auto summary = AnalyzeOldWaveformMonoInt16(mono.data(), frames);
		sink += summary.peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "old_waveform_average_s16", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunNewWaveformAverageBench() {
	constexpr int frames = 1 << 16;
	constexpr int iterations = 200;
	SyntheticInt16StereoProvider provider(frames);
	auto source = CreateAudioDisplaySource(&provider);
	std::vector<float> interleaved(static_cast<size_t>(frames) * provider.GetChannels());
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		source->GetFloatAudio(interleaved.data(), 0, frames);
		auto summary = AnalyzeWaveformInterleaved(interleaved.data(), frames, provider.GetChannels(), AudioMixPolicy::MonoAverage);
		sink += summary.peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "new_waveform_average_float", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunNewWaveformMaxAbsBench() {
	constexpr int frames = 1 << 16;
	constexpr int iterations = 200;
	SyntheticInt16StereoProvider provider(frames);
	auto source = CreateAudioDisplaySource(&provider);
	std::vector<float> interleaved(static_cast<size_t>(frames) * provider.GetChannels());
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		source->GetFloatAudio(interleaved.data(), 0, frames);
		auto summary = AnalyzeWaveformInterleaved(interleaved.data(), frames, provider.GetChannels(), AudioMixPolicy::MonoMaxAbs);
		sink += summary.peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(frames) * iterations;
	return { "new_waveform_maxabs_float", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunWaveformSummaryCacheColdBench() {
	constexpr int iterations = 200;
	SyntheticInt16StereoProvider provider(1 << 16);
	auto source = CreateAudioDisplaySource(&provider);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		AudioWaveformSummaryCache cache;
		cache.SetSource(source.get());
		cache.SetMillisecondsPerPixel(20.0);
		cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);
		auto const& block = cache.Get(0);
		sink += block.summaries[0].peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "waveform_summary_cache_cold", iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunWaveformSummaryCacheHotBench() {
	constexpr int iterations = 20000;
	SyntheticInt16StereoProvider provider(1 << 16);
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);
	auto const& warm = cache.Get(0);
	volatile float sink = warm.summaries[0].peak_max;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		auto const& block = cache.Get(0);
		sink += block.summaries[0].peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "waveform_summary_cache_hot", iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunWaveformSequentialPrefetchBench() {
	constexpr int blocks = 64;
	SyntheticInt16StereoProvider provider(1 << 22);
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < blocks; ++i) {
		auto const& block = cache.Get(i);
		sink += block.summaries[0].peak_max;
		cache.Prefetch(static_cast<size_t>(i + 1), static_cast<size_t>(i + 2));
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "waveform_sequential_prefetch", blocks, total_ms, total_ms / blocks, 0.0 };
}

BenchResult RunOldSpectrumBench() {
	constexpr int fft_size = 1024;
	constexpr int iterations = 2000;
	SyntheticInt16StereoProvider provider(fft_size);
	std::vector<int16_t> mono(fft_size);
	std::vector<float> fft_input(fft_size * 3);
	float *real = fft_input.data() + fft_size;
	float *imag = fft_input.data() + fft_size * 2;
	FFT fft;
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		provider.GetInt16MonoAudio(mono.data(), 0, fft_size);
		for (int j = 0; j < fft_size; ++j)
			fft_input[j] = mono[j] / 32768.0f;
		fft.Transform(fft_size, fft_input.data(), real, imag);
		float power = real[1] * real[1] + imag[1] * imag[1];
		sink += 10.0f * std::log10(power + 1e-12f);
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(fft_size) * iterations;
	return { "old_spectrum_block_s16", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunNewSpectrumBench() {
	constexpr int fft_size = 1024;
	constexpr int iterations = 2000;
	SyntheticInt16StereoProvider provider(fft_size);
	auto source = CreateAudioDisplaySource(&provider);
	std::vector<float> interleaved(static_cast<size_t>(fft_size) * provider.GetChannels());
	std::vector<float> mono(fft_size);
	std::vector<float> fft_input(fft_size * 3);
	float *real = fft_input.data() + fft_size;
	float *imag = fft_input.data() + fft_size * 2;
	FFT fft;
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		source->GetFloatAudio(interleaved.data(), 0, fft_size);
		MixAudioToMono(AudioMixPolicy::MonoAverage, interleaved.data(), fft_size, provider.GetChannels(), mono.data());
		std::copy(mono.begin(), mono.end(), fft_input.begin());
		fft.Transform(fft_size, fft_input.data(), real, imag);
		float power = real[1] * real[1] + imag[1] * imag[1];
		sink += 10.0f * std::log10(power + 1e-12f);
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double total_frames = static_cast<double>(fft_size) * iterations;
	return { "new_spectrum_block_float", iterations, total_ms, total_ms / iterations, total_frames / (total_ms / 1000.0) / 1'000'000.0 };
}

BenchResult RunSpectrumAnalysisCacheColdBench() {
	constexpr int iterations = 200;
	SyntheticInt16StereoProvider provider(1 << 16);
	auto source = CreateAudioDisplaySource(&provider);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		AudioSpectrumAnalysisCache cache;
		cache.SetSource(source.get());
		cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
		cache.SetResolution(9, 7);
		const float *block = cache.Get(0);
		sink += block[0];
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "spectrum_analysis_cache_cold", iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunSpectrumAnalysisCacheHotBench() {
	constexpr int iterations = 20000;
	SyntheticInt16StereoProvider provider(1 << 16);
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);
	const float *warm = cache.Get(0);
	volatile float sink = warm[0];

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i) {
		const float *block = cache.Get(0);
		sink += block[0];
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "spectrum_analysis_cache_hot", iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunSpectrumSequentialPrefetchBench() {
	constexpr int blocks = 64;
	SyntheticInt16StereoProvider provider(1 << 18);
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < blocks; ++i) {
		const float *block = cache.Get(i);
		sink += block[0];
		cache.Prefetch(static_cast<size_t>(i + 1), static_cast<size_t>(i + 2));
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "spectrum_sequential_prefetch", blocks, total_ms, total_ms / blocks, 0.0 };
}

float RunOldSpectrumRenderKernel(const std::vector<std::vector<float>> &blocks, int imgheight, int derivation_size, float amplitude_scale) {
	float sink = 0.f;
	int maxband = 1 << derivation_size;
	for (size_t ax = 0; ax < blocks.size(); ++ax) {
		const float *power = blocks[ax].data();
		if (imgheight > (1 << derivation_size)) {
			for (int y = 0; y < imgheight; ++y) {
				double ideal = static_cast<double>(y + 1.) / imgheight * maxband;
				float sample1 = power[static_cast<int>(std::floor(ideal))];
				float sample2 = power[static_cast<int>(std::ceil(ideal)) >= maxband ? maxband - 1 : static_cast<int>(std::ceil(ideal))];
				float frac = static_cast<float>(ideal - std::floor(ideal));
				sink += ((1 - frac) * sample1 + frac * sample2) * amplitude_scale;
			}
		}
		else {
			for (int y = 0; y < imgheight; ++y) {
				int sample1 = std::max(0, maxband * y / imgheight);
				int sample2 = std::min(maxband - 1, maxband * (y + 1) / imgheight);
				sink += *std::max_element(&power[sample1], &power[sample2 + 1]) * amplitude_scale;
			}
		}
	}
	return sink;
}

float RunNewSpectrumRenderKernel(const std::vector<std::vector<float>> &unique_blocks, const std::vector<size_t> &block_indices, int imgheight, int derivation_size, float amplitude_scale) {
	float sink = 0.f;
	bool interpolated = imgheight > (1 << derivation_size);
	std::vector<int> a(imgheight), b(imgheight);
	std::vector<float> frac(interpolated ? imgheight : 0);
	int maxband = 1 << derivation_size;
	if (interpolated) {
		for (int y = 0; y < imgheight; ++y) {
			double ideal = static_cast<double>(y + 1.) / imgheight * maxband;
			int lower = std::max(0, std::min(maxband - 1, static_cast<int>(std::floor(ideal))));
			int upper = std::max(0, std::min(maxband - 1, static_cast<int>(std::ceil(ideal))));
			a[y] = lower;
			b[y] = upper;
			frac[y] = static_cast<float>(ideal - std::floor(ideal));
		}
	}
	else {
		for (int y = 0; y < imgheight; ++y) {
			a[y] = std::max(0, maxband * y / imgheight);
			b[y] = std::min(maxband - 1, maxband * (y + 1) / imgheight);
		}
	}

	size_t last_index = static_cast<size_t>(-1);
	const float *power = nullptr;
	for (size_t ax = 0; ax < block_indices.size(); ++ax) {
		size_t idx = block_indices[ax];
		if (idx != last_index) {
			power = unique_blocks[idx].data();
			last_index = idx;
		}
		if (interpolated) {
			for (int y = 0; y < imgheight; ++y)
				sink += ((1 - frac[y]) * power[a[y]] + frac[y] * power[b[y]]) * amplitude_scale;
		}
		else {
			for (int y = 0; y < imgheight; ++y)
				sink += *std::max_element(&power[a[y]], &power[b[y] + 1]) * amplitude_scale;
		}
	}
	return sink;
}

float RunSpectrumVerticalZoomDragKernel(const std::vector<std::vector<float>> &unique_blocks, const std::vector<size_t> &block_indices, int imgheight, int derivation_size, const std::vector<float> &amplitude_scales, bool reuse_bands) {
	float sink = 0.f;
	const bool interpolated = imgheight > (1 << derivation_size);
	const int maxband = 1 << derivation_size;
	std::vector<int> a;
	std::vector<int> b;
	std::vector<float> frac;

	a.resize(imgheight);
	b.resize(imgheight);
	if (interpolated)
		frac.resize(imgheight);

	auto build_bands = [&]() {
		if (interpolated) {
			for (int y = 0; y < imgheight; ++y) {
				double ideal = static_cast<double>(y + 1.) / imgheight * maxband;
				a[y] = std::max(0, std::min(maxband - 1, static_cast<int>(std::floor(ideal))));
				b[y] = std::max(0, std::min(maxband - 1, static_cast<int>(std::ceil(ideal))));
				frac[y] = static_cast<float>(ideal - std::floor(ideal));
			}
		}
		else {
			for (int y = 0; y < imgheight; ++y) {
				a[y] = std::max(0, maxband * y / imgheight);
				b[y] = std::min(maxband - 1, maxband * (y + 1) / imgheight);
			}
		}
	};

	if (reuse_bands)
		build_bands();

	for (float amplitude_scale : amplitude_scales) {
		if (!reuse_bands)
			build_bands();

		size_t last_index = static_cast<size_t>(-1);
		const float *power = nullptr;
		for (size_t ax = 0; ax < block_indices.size(); ++ax) {
			size_t idx = block_indices[ax];
			if (idx != last_index) {
				power = unique_blocks[idx].data();
				last_index = idx;
			}
			if (interpolated) {
				for (int y = 0; y < imgheight; ++y)
					sink += ((1 - frac[y]) * power[a[y]] + frac[y] * power[b[y]]) * amplitude_scale;
			}
			else {
				for (int y = 0; y < imgheight; ++y)
					sink += *std::max_element(&power[a[y]], &power[b[y] + 1]) * amplitude_scale;
			}
		}
	}

	return sink;
}

BenchResult RunOldSpectrumRenderBench() {
	constexpr int iterations = 5000;
	constexpr int imgheight = 256;
	constexpr int derivation_size = 9;
	constexpr int columns = 32;
	std::vector<std::vector<float>> blocks(columns, std::vector<float>(1 << derivation_size));
	for (int c = 0; c < columns; ++c)
		for (int i = 0; i < (1 << derivation_size); ++i)
			blocks[c][i] = static_cast<float>((i + c) % 97) / 97.0f;
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		sink += RunOldSpectrumRenderKernel(blocks, imgheight, derivation_size, 1.0f);
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "old_spectrum_render_kernel", iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunNewSpectrumRenderOptimizedBench() {
	constexpr int iterations = 5000;
	constexpr int imgheight = 256;
	constexpr int derivation_size = 9;
	constexpr int unique_count = 8;
	constexpr int columns = 32;
	std::vector<std::vector<float>> unique_blocks(unique_count, std::vector<float>(1 << derivation_size));
	for (int c = 0; c < unique_count; ++c)
		for (int i = 0; i < (1 << derivation_size); ++i)
			unique_blocks[c][i] = static_cast<float>((i + c) % 97) / 97.0f;
	std::vector<size_t> block_indices(columns);
	for (int i = 0; i < columns; ++i)
		block_indices[i] = static_cast<size_t>((i / 4) % unique_count);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		sink += RunNewSpectrumRenderKernel(unique_blocks, block_indices, imgheight, derivation_size, 1.0f);
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "new_spectrum_render_kernel", iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunSpectrumVerticalZoomDragBench(bool reuse_bands, const char *name) {
	constexpr int iterations = 1000;
	constexpr int imgheight = 256;
	constexpr int derivation_size = 9;
	constexpr int unique_count = 8;
	constexpr int columns = 320;
	std::vector<std::vector<float>> unique_blocks(unique_count, std::vector<float>(1 << derivation_size));
	for (int c = 0; c < unique_count; ++c)
		for (int i = 0; i < (1 << derivation_size); ++i)
			unique_blocks[c][i] = static_cast<float>((i + c * 3) % 101) / 101.0f;
	std::vector<size_t> block_indices(columns);
	for (int i = 0; i < columns; ++i)
		block_indices[i] = static_cast<size_t>((i / 4) % unique_count);
	std::vector<float> amplitude_scales = {0.4f, 0.6f, 0.8f, 1.0f, 1.3f, 1.6f, 1.9f, 2.2f};
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < iterations; ++i)
		sink += RunSpectrumVerticalZoomDragKernel(unique_blocks, block_indices, imgheight, derivation_size, amplitude_scales, reuse_bands);
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { name, iterations, total_ms, total_ms / iterations, 0.0 };
}

BenchResult RunNaiveSpectrumVerticalZoomStreamBench() {
	constexpr int events = 300;
	constexpr int imgheight = 256;
	constexpr int derivation_size = 9;
	constexpr int unique_count = 8;
	constexpr int columns = 320;
	std::vector<std::vector<float>> unique_blocks(unique_count, std::vector<float>(1 << derivation_size));
	for (int c = 0; c < unique_count; ++c)
		for (int i = 0; i < (1 << derivation_size); ++i)
			unique_blocks[c][i] = static_cast<float>((i + c * 5) % 101) / 101.0f;
	std::vector<size_t> block_indices(columns);
	for (int i = 0; i < columns; ++i)
		block_indices[i] = static_cast<size_t>((i / 4) % unique_count);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < events; ++i) {
		std::vector<float> scales = { 0.4f + static_cast<float>(i % 12) * 0.1f };
		sink += RunSpectrumVerticalZoomDragKernel(unique_blocks, block_indices, imgheight, derivation_size, scales, true);
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "naive_spectrum_vertical_zoom_stream", events, total_ms, total_ms / events, 0.0 };
}

BenchResult RunCoalescedSpectrumVerticalZoomStreamBench() {
	constexpr int events = 300;
	constexpr int imgheight = 256;
	constexpr int derivation_size = 9;
	constexpr int unique_count = 8;
	constexpr int columns = 320;
	constexpr int interval_ms = 33;
	constexpr int event_spacing_ms = 6;
	std::vector<std::vector<float>> unique_blocks(unique_count, std::vector<float>(1 << derivation_size));
	for (int c = 0; c < unique_count; ++c)
		for (int i = 0; i < (1 << derivation_size); ++i)
			unique_blocks[c][i] = static_cast<float>((i + c * 5) % 101) / 101.0f;
	std::vector<size_t> block_indices(columns);
	for (int i = 0; i < columns; ++i)
		block_indices[i] = static_cast<size_t>((i / 4) % unique_count);
	volatile float sink = 0.f;

	int next_flush_ms = interval_ms;
	bool pending = false;
	float pending_scale = 1.0f;
	int executed = 0;
	auto t0 = clock_type::now();
	for (int i = 0; i < events; ++i) {
		int now_ms = i * event_spacing_ms;
		pending_scale = 0.4f + static_cast<float>(i % 12) * 0.1f;
		pending = true;
		while (now_ms >= next_flush_ms) {
			if (pending) {
				std::vector<float> scales = { pending_scale };
				sink += RunSpectrumVerticalZoomDragKernel(unique_blocks, block_indices, imgheight, derivation_size, scales, true);
				pending = false;
				++executed;
			}
			next_flush_ms += interval_ms;
		}
	}
	if (pending) {
		std::vector<float> scales = { pending_scale };
		sink += RunSpectrumVerticalZoomDragKernel(unique_blocks, block_indices, imgheight, derivation_size, scales, true);
		++executed;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "coalesced_spectrum_vertical_zoom_stream", executed, total_ms, executed ? total_ms / executed : 0.0, 0.0 };
}

BenchResult RunSpectrumSeekDragStreamBench() {
	constexpr int events = 2000;
	constexpr size_t viewport_blocks = 12;
	SyntheticInt16StereoProvider provider(1 << 24);
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	const size_t max_block = static_cast<size_t>(provider.GetNumSamples()) >> 7;
	size_t block = 0;
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < events; ++i) {
		if (i % 180 == 0)
			block = (block + 4000) % std::max<size_t>(1, max_block - viewport_blocks - 16);
		else
			block = (block + ((i & 1) ? 1 : 3)) % std::max<size_t>(1, max_block - viewport_blocks - 16);

		for (size_t b = block; b < block + viewport_blocks; ++b) {
			const float *p = cache.Get(b);
			sink += p[0];
		}
		cache.Prefetch(block + viewport_blocks, block + viewport_blocks + 8);
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "spectrum_seek_drag_stream", events, total_ms, total_ms / events, 0.0 };
}

BenchResult RunNaiveWaveformUpdateStreamBench() {
	constexpr int events = 300;
	constexpr int frames = 1 << 16;
	constexpr int channels = 2;
	std::vector<float> src(static_cast<size_t>(frames) * channels);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = static_cast<float>((static_cast<int>(i % 2048) - 1024) / 1024.0);
	volatile float sink = 0.f;

	auto t0 = clock_type::now();
	for (int i = 0; i < events; ++i) {
		auto summary = AnalyzeWaveformInterleaved(src.data(), frames, channels, AudioMixPolicy::MonoMaxAbs);
		sink += summary.peak_max;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "naive_waveform_update_stream", events, total_ms, total_ms / events, 0.0 };
}

BenchResult RunCoalescedWaveformUpdateStreamBench() {
	constexpr int events = 300;
	constexpr int frames = 1 << 16;
	constexpr int channels = 2;
	constexpr int interval_ms = 16;
	constexpr int event_spacing_ms = 6;
	std::vector<float> src(static_cast<size_t>(frames) * channels);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = static_cast<float>((static_cast<int>(i % 2048) - 1024) / 1024.0);
	volatile float sink = 0.f;

	int next_flush_ms = interval_ms;
	bool pending = false;
	int executed = 0;
	auto t0 = clock_type::now();
	for (int i = 0; i < events; ++i) {
		int now_ms = i * event_spacing_ms;
		pending = true;
		while (now_ms >= next_flush_ms) {
			if (pending) {
				auto summary = AnalyzeWaveformInterleaved(src.data(), frames, channels, AudioMixPolicy::MonoMaxAbs);
				sink += summary.peak_max;
				pending = false;
				++executed;
			}
			next_flush_ms += interval_ms;
		}
	}
	if (pending) {
		auto summary = AnalyzeWaveformInterleaved(src.data(), frames, channels, AudioMixPolicy::MonoMaxAbs);
		sink += summary.peak_max;
		++executed;
	}
	auto t1 = clock_type::now();
	(void)sink;

	double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	return { "coalesced_waveform_update_stream", executed, total_ms, executed ? total_ms / executed : 0.0, 0.0 };
}

std::string ToJson(const std::vector<BenchResult> &results) {
	std::ostringstream os;
	os << "{\n  \"benchmarks\": [\n";
	for (size_t i = 0; i < results.size(); ++i) {
		const auto &r = results[i];
		os << "    {\"name\":\"" << r.name << "\",\"iterations\":" << r.iterations
		   << ",\"total_ms\":" << std::fixed << std::setprecision(3) << r.total_ms
		   << ",\"avg_ms\":" << r.avg_ms
		   << ",\"throughput_mframes_s\":" << r.throughput_mframes_s << "}";
		if (i + 1 != results.size()) os << ",";
		os << "\n";
	}
	os << "  ]\n}\n";
	return os.str();
}
}

int main(int argc, char **argv) {
	std::string out_path;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--out" && i + 1 < argc)
			out_path = argv[++i];
	}

	wxInitializer initializer;
	if (!initializer.IsOk()) {
		std::cerr << "wxWidgets initialization failed\n";
		return 1;
	}

	std::vector<BenchResult> results;
	results.push_back(RunOldMonoFetchBench());
	results.push_back(RunDisplaySourceBench());
	results.push_back(RunLegacySingleChannelDisplaySourceBench());
	results.push_back(RunDirectSingleChannelDisplaySourceBench());
	results.push_back(RunMixBench(AudioMixPolicy::MonoAverage, "mix_mono_average"));
	results.push_back(RunMixBench(AudioMixPolicy::MonoMaxAbs, "mix_mono_maxabs"));
	results.push_back(RunOldWaveformBench());
	results.push_back(RunNewWaveformAverageBench());
	results.push_back(RunNewWaveformMaxAbsBench());
	results.push_back(RunWaveformSummaryCacheColdBench());
	results.push_back(RunWaveformSummaryCacheHotBench());
	results.push_back(RunWaveformSequentialPrefetchBench());
	results.push_back(RunWaveformBench());
	results.push_back(RunOldSpectrumBench());
	results.push_back(RunNewSpectrumBench());
	results.push_back(RunSpectrumAnalysisCacheColdBench());
	results.push_back(RunSpectrumAnalysisCacheHotBench());
	results.push_back(RunSpectrumSequentialPrefetchBench());
	results.push_back(RunOldSpectrumRenderBench());
	results.push_back(RunNewSpectrumRenderOptimizedBench());
	results.push_back(RunAudioRendererTileDrawHotBench(16, "audio_renderer_draw_hot_w16"));
	results.push_back(RunAudioRendererTileDrawHotBench(32, "audio_renderer_draw_hot_w32"));
	results.push_back(RunAudioRendererTileDrawHotBench(64, "audio_renderer_draw_hot_w64"));
	results.push_back(RunAudioRendererTileDrawHotBench(128, "audio_renderer_draw_hot_w128"));
	results.push_back(RunAudioRendererTileDrawColdBench(16, "audio_renderer_draw_cold_w16"));
	results.push_back(RunAudioRendererTileDrawColdBench(32, "audio_renderer_draw_cold_w32"));
	results.push_back(RunAudioRendererTileDrawColdBench(64, "audio_renderer_draw_cold_w64"));
	results.push_back(RunAudioRendererTileDrawColdBench(128, "audio_renderer_draw_cold_w128"));
	results.push_back(RunSpectrumVerticalZoomDragBench(false, "spectrum_vertical_zoom_drag_uncached_bands"));
	results.push_back(RunSpectrumVerticalZoomDragBench(true, "spectrum_vertical_zoom_drag_cached_bands"));
	results.push_back(RunNaiveSpectrumVerticalZoomStreamBench());
	results.push_back(RunCoalescedSpectrumVerticalZoomStreamBench());
	results.push_back(RunSpectrumSeekDragStreamBench());
	results.push_back(RunNaiveWaveformUpdateStreamBench());
	results.push_back(RunCoalescedWaveformUpdateStreamBench());

	auto json = ToJson(results);
	std::cout << json;
	if (!out_path.empty()) {
		std::ofstream out(out_path, std::ios::binary);
		out << json;
	}
	return 0;
}
