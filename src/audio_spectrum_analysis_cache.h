#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio_display_source.h"
#include "audio_mix_policy.h"

#ifdef WITH_PFFFT
#include <pffft/pffft.h>
#endif

class AudioLatestRangeScheduler;

#ifdef WITH_FFTW3
namespace audio::spectrum {
class Fftw3SpectrumTransform;
}
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
	size_t cache_budget_bytes = 0;
};

enum class SpectrumCacheFormat {
	Float32,
	HalfFloat,
};

class AudioSpectrumAnalysisCache {
public:
	using BlockHandle = std::shared_ptr<float const[]>;

private:
	using MutableBlock = std::shared_ptr<float[]>;

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
	size_t cache_block_bytes = sizeof(float);
	size_t max_cache_bytes = 64 * 1024 * 1024;
	size_t current_cache_bytes = 0;
	size_t current_cache_entries = 0;
	uint64_t touch_counter = 0;

	mutable std::mutex cache_mutex;
	std::mutex build_mutex;
	std::unordered_map<size_t, BlockHandle> cache_blocks;
	std::unordered_map<size_t, uint64_t> cache_touch;
	// Blocks already rebuilt once as pure silence in this generation. The next
	// silent rebuild is retained so genuine digital silence stays cached while
	// a transient zero-fill still gets one chance to recover.
	std::unordered_set<size_t> silent_rebuild_blocks;
	std::priority_queue<TouchEntry, std::vector<TouchEntry>, std::greater<TouchEntry>> touch_heap;

	std::vector<float> audio_scratch;
	std::vector<float> mono_scratch;
	bool rolling_window_valid = false;
	size_t rolling_window_block_index = 0;

#ifdef WITH_FFTW3
	std::unique_ptr<audio::spectrum::Fftw3SpectrumTransform> fftw3_transform;
#endif

#ifdef WITH_PFFFT
	PFFFT_Setup *pffft_setup = nullptr;
	float *pffft_input = nullptr;
	float *pffft_output = nullptr;
	float *pffft_work = nullptr;
#endif
	std::vector<float> fft_scratch;

	uint64_t metrics_generation = 0;
	uint64_t metrics_cache_hits = 0;
	uint64_t metrics_cache_misses = 0;
	uint64_t metrics_visible_builds = 0;
	uint64_t metrics_visible_lock_contention = 0;
	uint64_t metrics_prefetch_requests = 0;
	uint64_t metrics_prefetch_builds = 0;
	uint64_t metrics_prefetch_busy_skips = 0;
	uint64_t metrics_stale_drops = 0;
	uint64_t metrics_evictions = 0;
	std::atomic<bool> prefetch_enabled { true };
	std::atomic<size_t> prefetch_build_max_blocks { 64 };
	std::atomic<uint32_t> visible_waiters { 0 };

	std::mutex scheduler_mutex;
	std::unique_ptr<AudioLatestRangeScheduler> scheduler;
	std::atomic<uint64_t> active_prefetch_generation { 0 };
	bool has_active_prefetch_range = false;
	size_t active_prefetch_first = 0;
	size_t active_prefetch_last = 0;

	mutable std::mutex ready_callback_mutex;
	std::function<void()> ready_callback;

	size_t BinCount() const { return static_cast<size_t>(1) << derivation_size; }
	size_t WindowSampleCount() const { return static_cast<size_t>(2) << derivation_size; }
	size_t HopSampleCount() const { return static_cast<size_t>(1) << derivation_dist; }
	size_t BlockBytes() const { return cache_block_bytes; }

	void RecreateCache();
	void StopScheduler();
	void DestroyFftResources();
	MutableBlock BuildBlock(size_t block_index);
	void ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation);
	bool IsCurrentPrefetchGeneration(uint64_t generation) const;
	void NotifyReady() const;
	void TouchLocked(size_t block_index);
	void TrimLocked();
	bool ShouldDeferSilentBlockLocked(size_t block_index, float const *block);
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
	BlockHandle Get(size_t block_index);
	BlockHandle GetIfReady(size_t block_index);
	bool AreBlocksReady(size_t first_block, size_t last_block);
	void Prefetch(size_t first_block, size_t last_block);
	void SetPrefetchEnabled(bool enabled);
	void SetPrefetchBuildMaxBlocks(size_t max_blocks);
	void SetReadyCallback(std::function<void()> callback);
	AudioSpectrumAnalysisCacheMetrics GetMetricsSnapshot() const;
};
