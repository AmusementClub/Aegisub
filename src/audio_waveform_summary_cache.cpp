#include "audio_waveform_summary_cache.h"

#include <libaegisub/make_unique.h>

#include <algorithm>

AudioWaveformSummaryCacheBlockFactory::AudioWaveformSummaryCacheBlockFactory(AudioWaveformSummaryCache *cache)
: cache(cache) {
}

std::unique_ptr<AudioWaveformSummaryBlock> AudioWaveformSummaryCacheBlockFactory::ProduceBlock(size_t i) {
	auto block = agi::make_unique<AudioWaveformSummaryBlock>();
	cache->FillBlock(i, *block);
	return block;
}

size_t AudioWaveformSummaryCacheBlockFactory::GetBlockSize() const {
	return sizeof(AudioWaveformSummaryBlock);
}

AudioWaveformSummaryCache::AudioWaveformSummaryCache()
: cache(agi::make_unique<Cache>(0, AudioWaveformSummaryCacheBlockFactory(this))) {
}

void AudioWaveformSummaryCache::RecreateCache() {
	if (!source || pixel_ms <= 0.0 || source->GetSampleRate() <= 0 || source->GetNumSamples() <= 0) {
		cache->SetBlockCount(0);
		audio_buffer.clear();
		return;
	}

	const double duration = source->GetNumSamples() * 1000.0 / source->GetSampleRate();
	size_t block_count = static_cast<size_t>(duration / pixel_ms / AudioWaveformSummaryBlock::width);
	if (block_count == 0)
		block_count = 1;
	cache->SetBlockCount(block_count);
	audio_buffer.clear();
}

void AudioWaveformSummaryCache::FillBlock(size_t block_index, AudioWaveformSummaryBlock &block) {
	if (!source || pixel_ms <= 0.0) {
		for (auto &summary : block.summaries)
			summary = AudioWaveformSummary();
		return;
	}

	const int channels = std::max(1, source->GetChannels());
	const double pixel_samples = pixel_ms * source->GetSampleRate() / 1000.0;
	const int samples_per_pixel = std::max(1, static_cast<int>(pixel_samples));
	const size_t block_frames = AudioWaveformSummaryBlock::width * static_cast<size_t>(samples_per_pixel);
	audio_buffer.resize(block_frames * channels);

	const int64_t block_start = static_cast<int64_t>(block_index * AudioWaveformSummaryBlock::width * pixel_samples);
	source->GetFloatAudio(audio_buffer.data(), block_start, static_cast<int64_t>(block_frames));

	const float *cur = audio_buffer.data();
	for (auto &summary : block.summaries) {
		summary = AnalyzeWaveformInterleaved(cur, samples_per_pixel, channels, mix_policy);
		cur += static_cast<size_t>(samples_per_pixel) * channels;
	}
}

void AudioWaveformSummaryCache::SetSource(AudioDisplaySource *new_source) {
	if (source == new_source)
		return;
	source = new_source;
	RecreateCache();
}

void AudioWaveformSummaryCache::SetMillisecondsPerPixel(double new_pixel_ms) {
	if (pixel_ms == new_pixel_ms)
		return;
	pixel_ms = new_pixel_ms;
	RecreateCache();
}

void AudioWaveformSummaryCache::SetMixPolicy(AudioMixPolicy new_policy) {
	if (mix_policy == new_policy)
		return;
	mix_policy = new_policy;
	RecreateCache();
}

void AudioWaveformSummaryCache::Age(size_t max_size) {
	cache->Age(max_size);
}

bool AudioWaveformSummaryCache::IsReady() const {
	return source && pixel_ms > 0.0;
}

const AudioWaveformSummaryBlock& AudioWaveformSummaryCache::Get(size_t block_index) {
	return cache->Get(block_index);
}
