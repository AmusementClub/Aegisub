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
#include "audio_latest_range_scheduler.h"
#include "audio_mix_policy.h"

struct AudioWaveformSummaryBlock {
	static constexpr size_t width = 32;
	std::array<AudioWaveformSummary, width> summaries;
};

struct AudioWaveformSummaryCacheMetrics {
	uint64_t generation = 0;
	uint64_t cache_hits = 0;
	uint64_t cache_misses = 0;
	uint64_t visible_builds = 0;
	uint64_t prefetch_requests = 0;
	uint64_t prefetch_builds = 0;
	uint64_t stale_drops = 0;
	uint64_t evictions = 0;
	size_t cache_entries = 0;
	size_t cache_bytes = 0;
};

class AudioWaveformSummaryCache {
	struct TouchEntry {
		uint64_t touch = 0;
		size_t index = 0;
		bool operator>(const TouchEntry &other) const {
			return touch > other.touch;
		}
	};

	AudioDisplaySource *source = nullptr;
	double pixel_ms = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoMaxAbs;
	size_t block_count = 0;
	size_t max_cache_bytes = 4 * 1024 * 1024;
	size_t current_cache_bytes = 0;
	uint64_t touch_counter = 0;

	mutable std::mutex cache_mutex;
	std::vector<std::unique_ptr<AudioWaveformSummaryBlock>> cache_blocks;
	std::vector<uint64_t> cache_touch;
	std::vector<uint8_t> pending_blocks;
	std::priority_queue<TouchEntry, std::vector<TouchEntry>, std::greater<TouchEntry>> touch_heap;

	std::mutex ready_mutex;
	std::vector<std::pair<size_t, std::unique_ptr<AudioWaveformSummaryBlock>>> ready_blocks;
	std::atomic<bool> has_ready_blocks{false};

	std::unique_ptr<AudioLatestRangeScheduler> scheduler;
 	mutable std::mutex scheduler_mutex;

	std::atomic<uint64_t> metrics_generation{0};
	std::atomic<uint64_t> metrics_cache_hits{0};
	std::atomic<uint64_t> metrics_cache_misses{0};
	std::atomic<uint64_t> metrics_visible_builds{0};
	std::atomic<uint64_t> metrics_prefetch_requests{0};
	std::atomic<uint64_t> metrics_prefetch_builds{0};
	std::atomic<bool> prefetch_enabled{true};
	std::atomic<uint64_t> active_prefetch_generation{0};
	std::atomic<uint64_t> metrics_stale_drops{0};
	std::atomic<uint64_t> metrics_evictions{0};
	std::function<void()> ready_callback;

	void StopScheduler();
	bool IsCurrentPrefetchGeneration(uint64_t generation) const;
	void RecreateCache();
	std::unique_ptr<AudioWaveformSummaryBlock> BuildBlock(size_t block_index) const;
	size_t GetMaxBuildBlocks(size_t preferred_cap) const;
	std::vector<std::pair<size_t, std::unique_ptr<AudioWaveformSummaryBlock>>> BuildBlocks(size_t first_block, size_t last_block) const;
	void TouchLocked(size_t block_index);
	void TrimLocked();
	void DrainReady();
	void ClearPendingRange(size_t first_block, size_t last_block);
	void ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation);

public:
	AudioWaveformSummaryCache();
	~AudioWaveformSummaryCache();

	void SetSource(AudioDisplaySource *new_source);
	void SetMillisecondsPerPixel(double new_pixel_ms);
	void SetMixPolicy(AudioMixPolicy new_policy);
	void Age(size_t max_size);
	bool IsReady() const;
	const AudioWaveformSummaryBlock& Get(size_t block_index);
	const AudioWaveformSummaryBlock* GetIfReady(size_t block_index);
	bool AreBlocksReady(size_t first_block, size_t last_block);
	void Prefetch(size_t first_block, size_t last_block);
	void SetPrefetchEnabled(bool enabled);
	void SetReadyCallback(std::function<void()> callback) { ready_callback = std::move(callback); }
	AudioWaveformSummaryCacheMetrics GetMetricsSnapshot() const;
};
