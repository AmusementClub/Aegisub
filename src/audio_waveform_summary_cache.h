#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "audio_display_analysis.h"
#include "audio_display_source.h"
#include "audio_mix_policy.h"

class AudioLatestRangeScheduler;

struct AudioWaveformPcm16Summary {
	int peak_min = 0;
	int peak_max = 0;
	int64_t avg_min_accum = 0;
	int64_t avg_max_accum = 0;
};

struct AudioWaveformSummaryBlock {
	static constexpr size_t width = 32;
	std::array<AudioWaveformSummary, width> summaries;
	std::array<AudioWaveformPcm16Summary, width> pcm16_summaries;
	bool has_exact_pcm16 = false;
};

struct AudioWaveformSummaryCacheMetrics {
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
	size_t cache_touch_entries = 0;
};

class AudioWaveformSummaryCache {
public:
	using BlockHandle = std::shared_ptr<AudioWaveformSummaryBlock const>;

private:
	using MutableBlock = std::shared_ptr<AudioWaveformSummaryBlock>;
	using BuiltBlocks = std::vector<std::pair<size_t, MutableBlock>>;

	struct TouchEntry {
		uint64_t touch = 0;
		size_t index = 0;
		bool operator>(TouchEntry const& other) const { return touch > other.touch; }
	};

	AudioDisplaySource *source = nullptr;
	double pixel_ms = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoMaxAbs;
	size_t block_count = 0;
	size_t max_cache_bytes = 4 * 1024 * 1024;
	size_t current_cache_bytes = 0;
	size_t current_cache_entries = 0;
	uint64_t touch_counter = 0;

	mutable std::mutex cache_mutex;
	std::mutex build_mutex;
	std::vector<BlockHandle> cache_blocks;
	std::vector<uint64_t> cache_touch;
	std::priority_queue<TouchEntry, std::vector<TouchEntry>, std::greater<TouchEntry>> touch_heap;

	std::mutex scheduler_mutex;
	std::unique_ptr<AudioLatestRangeScheduler> scheduler;
	std::atomic<uint64_t> active_prefetch_generation { 0 };
	bool has_active_prefetch_range = false;
	size_t active_prefetch_first = 0;
	size_t active_prefetch_last = 0;

	std::atomic<bool> prefetch_enabled { true };
	std::atomic<size_t> prefetch_build_max_blocks { 64 };
	std::atomic<uint32_t> visible_waiters { 0 };

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

	mutable std::mutex ready_callback_mutex;
	std::function<void()> ready_callback;

	void StopScheduler();
	bool IsCurrentPrefetchGeneration(uint64_t generation) const;
	void NotifyReady() const;
	void RecreateCache();
	BuiltBlocks BuildBlocks(size_t first_block, size_t last_block, uint64_t generation = 0) const;
	void TouchLocked(size_t block_index);
	void CompactTouchHeapLocked();
	void TrimLocked();
	void ClearLocked();
	void ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation);

public:
	AudioWaveformSummaryCache();
	~AudioWaveformSummaryCache();

	void SetSource(AudioDisplaySource *new_source);
	void SetMillisecondsPerPixel(double new_pixel_ms);
	void SetMixPolicy(AudioMixPolicy new_policy);
	void Age(size_t max_size);
	bool IsReady() const;
	BlockHandle Get(size_t block_index);
	BlockHandle GetIfReady(size_t block_index);
	bool AreBlocksReady(size_t first_block, size_t last_block);
	void Prefetch(size_t first_block, size_t last_block);
	void SetPrefetchEnabled(bool enabled);
	void SetPrefetchBuildMaxBlocks(size_t max_blocks);
	void SetReadyCallback(std::function<void()> callback);
	AudioWaveformSummaryCacheMetrics GetMetricsSnapshot() const;
};
