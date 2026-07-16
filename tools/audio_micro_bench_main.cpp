#include <benchmark/benchmark.h>

#include "audio_display_analysis.h"
#include "audio_mix_policy.h"
#include "skia/audio/skia_audio_content.h"
#include "skia/audio/skia_audio_frame_model.h"

#include <memory>
#include <vector>

using namespace aegisub::skia::audio;

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

static void BM_SkiaFrameViewport(benchmark::State &state) {
	FrameViewportRequest request;
	request.logical_width = static_cast<int>(state.range(0));
	request.logical_height = 586;
	request.content_scale = 1.0;
	request.timeline_height = 20;
	request.scrollbar_height = 15;
	request.scroll_left = 48000;
	request.duration_ms = 45 * 60 * 1000;
	request.milliseconds_per_logical_pixel = AudioMillisecondsPerLogicalPixel(0);
	for (auto _ : state)
		benchmark::DoNotOptimize(BuildFrameViewport(request));
}
BENCHMARK(BM_SkiaFrameViewport)->Arg(640)->Arg(1144)->Arg(1920);

static void BM_SkiaVisibleTilePlan(benchmark::State &state) {
	ContentViewportRequest request;
	request.generation = { 1, 1 };
	request.kind = ContentKind::Waveform;
	request.first_column = 48000;
	request.column_count = static_cast<std::uint32_t>(state.range(0));
	request.tile_column_count = 256;
	for (auto _ : state)
		benchmark::DoNotOptimize(PlanVisibleContentTiles(request));
}
BENCHMARK(BM_SkiaVisibleTilePlan)->Arg(640)->Arg(1144)->Arg(1920);

static void BM_SkiaStyleSpanPlan(benchmark::State &state) {
	FrameViewportRequest viewport_request;
	viewport_request.logical_width = 1144;
	viewport_request.logical_height = 586;
	viewport_request.content_scale = 1.0;
	viewport_request.timeline_height = 20;
	viewport_request.scrollbar_height = 15;
	viewport_request.scroll_left = 48000;
	viewport_request.duration_ms = 45 * 60 * 1000;
	viewport_request.milliseconds_per_logical_pixel = AudioMillisecondsPerLogicalPixel(0);
	auto const viewport = BuildFrameViewport(viewport_request);

	std::vector<TimeStyleRange> ranges;
	ranges.reserve(static_cast<std::size_t>(state.range(0)));
	for (int i = 0; i < state.range(0); ++i) {
		auto const start = i * 1000;
		ranges.push_back({ start, start + 1500, static_cast<FrameStyle>(i % 4) });
	}
	for (auto _ : state)
		benchmark::DoNotOptimize(BuildDeviceStyleSpans(ranges, viewport));
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_SkiaStyleSpanPlan)->Arg(32)->Arg(256)->Arg(1024);

static void BM_SkiaContentStoreFind(benchmark::State &state) {
	ContentTileStore store(4 * 1024 * 1024);
	auto tile = std::make_shared<ContentTile>();
	tile->key = { { 1, 1 }, ContentKind::Waveform, 0, 256, 0 };
	tile->waveform.resize(tile->key.column_count);
	store.ResetGeneration(tile->key.generation);
	if (store.Publish(tile) != ContentPublishResult::Accepted) {
		state.SkipWithError("could not seed content store");
		return;
	}
	auto key = tile->key;
	if (state.range(0) == 0)
		key.tile_index = 1;
	for (auto _ : state)
		benchmark::DoNotOptimize(store.Find(key));
}
BENCHMARK(BM_SkiaContentStoreFind)->Arg(1)->Arg(0);

static void BM_SkiaSpectrumBandPlan(benchmark::State &state) {
	SpectrumBandPlanRequest request;
	request.bin_count = 1024;
	request.output_height = static_cast<int>(state.range(0));
	request.sample_rate = 48000;
	request.mode = SpectrumScaleMode::FrequencyCurve;
	for (auto _ : state)
		benchmark::DoNotOptimize(BuildSpectrumBandPlan(request));
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * request.output_height);
}
BENCHMARK(BM_SkiaSpectrumBandPlan)->Arg(128)->Arg(512)->Arg(1080);

BENCHMARK_MAIN();
