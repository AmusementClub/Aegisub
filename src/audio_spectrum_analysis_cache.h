#pragma once

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "audio_display_source.h"
#include "audio_latest_range_scheduler.h"
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

/// Storage format for spectrum analysis cache blocks.
enum class SpectrumCacheFormat {
	Float32,    ///< 32-bit float per bin (default, no precision loss, 2x memory)
	HalfFloat,  ///< 16-bit half-float per bin (saves memory, slight precision loss)
};

class AudioSpectrumAnalysisCache {
	/// Opaque byte storage for a cache block.
	/// Actual element type depends on cache_format:
	///   Float32  → float[]  (4 bytes per bin)
	///   HalfFloat → uint16_t[] (2 bytes per bin)
	using CacheBlock = std::unique_ptr<uint8_t[]>;

	struct TouchEntry {
		uint64_t touch = 0;
		size_t index = 0;
		bool operator>(const TouchEntry &other) const {
			return touch > other.touch;
		}
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
	std::vector<CacheBlock> cache_blocks;
	std::vector<uint64_t> cache_touch;
	std::vector<uint8_t> pending_blocks;
	std::priority_queue<TouchEntry, std::vector<TouchEntry>, std::greater<TouchEntry>> touch_heap;
	mutable std::mutex build_mutex;

	std::mutex ready_mutex;
	std::vector<std::pair<size_t, CacheBlock>> ready_blocks;
	std::atomic<bool> has_ready_blocks{false};
	mutable std::mutex scheduler_mutex;

	std::vector<float> audio_scratch;
	std::vector<float> mono_scratch;
	mutable std::vector<float> decoded_block_scratch;
	mutable size_t decoded_block_index = static_cast<size_t>(-1);
	bool rolling_window_valid = false;
	size_t rolling_window_block_index = 0;

	/// Bytes per element in a cache block (4 for Float32, 2 for HalfFloat).
	size_t BlockElementSize() const { return cache_format == SpectrumCacheFormat::Float32 ? sizeof(float) : sizeof(uint16_t); }
	/// Total bytes per cache block.
	size_t BlockBytes() const { return BlockElementSize() << derivation_size; }

#ifdef WITH_FFTW3
	fftw_plan dft_plan = nullptr;
	double *dft_input = nullptr;
	fftw_complex *dft_output = nullptr;
#else
	std::vector<float> fft_scratch;
#endif

	std::atomic<uint64_t> metrics_generation{0};
	std::atomic<uint64_t> metrics_cache_hits{0};
	std::atomic<uint64_t> metrics_cache_misses{0};
	std::atomic<uint64_t> metrics_visible_builds{0};
	std::atomic<uint64_t> metrics_visible_lock_contention{0};
	std::atomic<uint64_t> metrics_prefetch_requests{0};
	std::atomic<uint64_t> metrics_prefetch_builds{0};
	std::atomic<uint64_t> metrics_prefetch_busy_skips{0};
	std::atomic<bool> prefetch_enabled{true};
	std::atomic<size_t> prefetch_build_max_blocks{1024};
	std::atomic<uint64_t> active_prefetch_generation{0};
	std::atomic<uint64_t> metrics_stale_drops{0};
	std::atomic<uint64_t> metrics_evictions{0};
	std::function<void()> ready_callback;

	std::unique_ptr<AudioLatestRangeScheduler> scheduler;

	void StopScheduler();
	bool IsCurrentPrefetchGeneration(uint64_t generation) const;
	void RecreateCache();
	CacheBlock BuildBlockUnlocked(size_t block_index);
	size_t GetMaxBuildBlocks(size_t preferred_cap) const;
	std::vector<std::pair<size_t, CacheBlock>> BuildBlocksUnlockedInternal(size_t first_block, size_t last_block, uint64_t generation, bool check_generation);
	std::vector<std::pair<size_t, CacheBlock>> BuildBlocksUnlocked(size_t first_block, size_t last_block);
	std::vector<std::pair<size_t, CacheBlock>> BuildBlocksUnlocked(size_t first_block, size_t last_block, uint64_t generation);
	void TouchLocked(size_t block_index);
	void TrimLocked();
	void DrainReady();
	void ClearPendingRange(size_t first_block, size_t last_block);
	void ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation);
	const float* DecodeBlock(const uint8_t *block, size_t block_index) const;

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
	void SetPrefetchBuildMaxBlocks(size_t max_blocks) { prefetch_build_max_blocks.store(std::max<size_t>(1, max_blocks), std::memory_order_relaxed); }
	void SetReadyCallback(std::function<void()> callback) { ready_callback = std::move(callback); }
	AudioSpectrumAnalysisCacheMetrics GetMetricsSnapshot() const;
};
