#include <benchmark/benchmark.h>

#include "audio_display_analysis.h"
#include "audio_mix_policy.h"

#include <vector>

static void BM_MixMonoAverage(benchmark::State &state) {
	const int frames = static_cast<int>(state.range(0));
	const int channels = 6;
	std::vector<float> src(static_cast<size_t>(frames) * channels, 0.5f);
	std::vector<float> dst(frames);
	for (auto _ : state)
		MixAudioToMono(AudioMixPolicy::MonoAverage, src.data(), frames, channels, dst.data());
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * frames);
}
BENCHMARK(BM_MixMonoAverage)->Arg(1 << 12)->Arg(1 << 16);

static void BM_WaveformAnalysis(benchmark::State &state) {
	const int frames = static_cast<int>(state.range(0));
	const int channels = 6;
	std::vector<float> src(static_cast<size_t>(frames) * channels, 0.25f);
	for (auto _ : state)
		benchmark::DoNotOptimize(AnalyzeWaveformInterleaved(src.data(), frames, channels, AudioMixPolicy::MonoMaxAbs));
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * frames);
}
BENCHMARK(BM_WaveformAnalysis)->Arg(1 << 12)->Arg(1 << 16);

BENCHMARK_MAIN();
