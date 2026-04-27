#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "audio_display_source.h"
#include "audio_mix_policy.h"

#ifdef WITH_FFTW3
#include <fftw3.h>
#endif

struct AudioSpectrumAnalysisCacheMetrics {
	uint64_t generation = 0;
	uint64_t cache_hits = 0;
	uint64_t cache_misses = 0;
	uint64_t visible_builds = 0;
	uint64_t visible_lock_contention = 0;
	uint64_t prefetch_requests = 0;
	uint64_t prefetch_builds = 0;
	uint64_t prefetch_busy_skips = 0;
	bool prefetch_enabled = true;
	uint64_t stale_drops = 0;
	uint64_t evictions = 0;
	size_t cache_entries = 0;
	size_t cache_bytes = 0;
};

enum class SpectrumCacheFormat {
	Float32,
	HalfFloat,
};

class AudioSpectrumAnalysisCache {
	using CacheBlock = std::unique_ptr<float[]>;

	struct TouchEntry {
		uint64_t touch = 0;
		size_t index = 0;
		bool operator>(const TouchEntry &other) const { return touch > other.touch; }
	};

	AudioDisplaySource *source = nullptr;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoAverage;
	SpectrumCacheFormat cache_format = SpectrumCacheFormat::Float32;
	size_t derivation_size = 0;
	size_t derivation_dist = 0;
	size_t block_count = 0;
	size_t max_cache_bytes = 64 * 1024 * 1024;
	size_t current_cache_bytes = 0;
	size_t current_cache_entries = 0;
	uint64_t touch_counter = 0;

	mutable std::mutex cache_mutex;
	std::mutex build_mutex;
	std::unordered_map<size_t, CacheBlock> cache_blocks;
	std::unordered_map<size_t, uint64_t> cache_touch;
	std::priority_queue<TouchEntry, std::vector<TouchEntry>, std::greater<TouchEntry>> touch_heap;

	std::vector<float> audio_scratch;
	std::vector<float> mono_scratch;
	bool rolling_window_valid = false;
	size_t rolling_window_block_index = 0;

#ifdef WITH_FFTW3
	fftw_plan dft_plan = nullptr;
	double *dft_input = nullptr;
	fftw_complex *dft_output = nullptr;
#else
	std::vector<float> fft_scratch;
#endif

	uint64_t metrics_generation = 0;
	uint64_t metrics_cache_hits = 0;
	uint64_t metrics_cache_misses = 0;
	uint64_t metrics_visible_builds = 0;
	uint64_t metrics_prefetch_requests = 0;
	uint64_t metrics_evictions = 0;
	bool prefetch_enabled = true;
	std::function<void()> ready_callback;

	size_t BinCount() const { return static_cast<size_t>(1) << derivation_size; }
	size_t WindowSampleCount() const { return static_cast<size_t>(2) << derivation_size; }
	size_t HopSampleCount() const { return static_cast<size_t>(1) << derivation_dist; }
	size_t BlockBytes() const { return sizeof(float) * BinCount(); }

	void RecreateCache();
	CacheBlock BuildBlock(size_t block_index);
	void TouchLocked(size_t block_index);
	void TrimLocked();
	void ClearLocked();

public:
	AudioSpectrumAnalysisCache();
	~AudioSpectrumAnalysisCache();

	void SetSource(AudioDisplaySource *new_source);
	void SetMixPolicy(AudioMixPolicy new_policy);
	void SetCacheFormat(SpectrumCacheFormat format);
	SpectrumCacheFormat GetCacheFormat() const { return cache_format; }
	void SetResolution(size_t new_derivation_size, size_t new_derivation_dist);
	void Age(size_t max_size);
	bool IsReady() const;
	const float* Get(size_t block_index);
	const float* GetIfReady(size_t block_index);
	bool AreBlocksReady(size_t first_block, size_t last_block);
	void Prefetch(size_t first_block, size_t last_block);
	void SetPrefetchEnabled(bool enabled);
	void SetPrefetchBuildMaxBlocks(size_t) { }
	void SetReadyCallback(std::function<void()> callback) { ready_callback = std::move(callback); }
	AudioSpectrumAnalysisCacheMetrics GetMetricsSnapshot() const;
};
