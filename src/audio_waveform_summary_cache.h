#pragma once

#include <array>
#include <cassert>
#include <memory>
#include <vector>

#include "audio_display_analysis.h"
#include "audio_display_source.h"
#include "audio_mix_policy.h"
#include "block_cache.h"

struct AudioWaveformSummaryBlock {
	static constexpr size_t width = 32;
	std::array<AudioWaveformSummary, width> summaries;
};

class AudioWaveformSummaryCache;

class AudioWaveformSummaryCacheBlockFactory {
	AudioWaveformSummaryCache *cache;

public:
	typedef std::unique_ptr<AudioWaveformSummaryBlock> BlockType;

	AudioWaveformSummaryCacheBlockFactory(AudioWaveformSummaryCache *cache = nullptr);
	std::unique_ptr<AudioWaveformSummaryBlock> ProduceBlock(size_t i);
	size_t GetBlockSize() const;
};

class AudioWaveformSummaryCache {
	friend class AudioWaveformSummaryCacheBlockFactory;

	AudioDisplaySource *source = nullptr;
	double pixel_ms = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoMaxAbs;
	std::vector<float> audio_buffer;

	using Cache = DataBlockCache<AudioWaveformSummaryBlock, 8, AudioWaveformSummaryCacheBlockFactory>;
	std::unique_ptr<Cache> cache;

	void RecreateCache();
	void FillBlock(size_t block_index, AudioWaveformSummaryBlock &block);

public:
	AudioWaveformSummaryCache();

	void SetSource(AudioDisplaySource *new_source);
	void SetMillisecondsPerPixel(double new_pixel_ms);
	void SetMixPolicy(AudioMixPolicy new_policy);
	void Age(size_t max_size);
	bool IsReady() const;
	const AudioWaveformSummaryBlock& Get(size_t block_index);
};
