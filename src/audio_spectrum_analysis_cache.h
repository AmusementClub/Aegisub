#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "audio_display_source.h"
#include "audio_latest_range_scheduler.h"
#include "audio_mix_policy.h"

struct AudioSpectrumAnalysisCacheMetrics {
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

class AudioSpectrumAnalysisCache {
	AudioDisplaySource *source = nullptr;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoAverage;
	size_t derivation_size = 0;
	size_t derivation_dist = 0;
	size_t block_count = 0;
	size_t max_cache_bytes = 64 * 1024 * 1024;
	size_t current_cache_bytes = 0;
	uint64_t touch_counter = 0;

	mutable std::mutex cache_mutex;
	std::vector<std::unique_ptr<float[]>> cache_blocks;
	std::vector<uint64_t> cache_touch;

	std::mutex ready_mutex;
	std::vector<std::pair<size_t, std::unique_ptr<float[]>>> ready_blocks;
	std::atomic<bool> has_ready_blocks{false};

	std::unique_ptr<AudioLatestRangeScheduler> scheduler;

	std::vector<float> audio_scratch;
	std::vector<float> mono_scratch;
	bool rolling_window_valid = false;
	size_t rolling_window_block_index = 0;

	std::atomic<uint64_t> metrics_generation{0};
	std::atomic<uint64_t> metrics_cache_hits{0};
	std::atomic<uint64_t> metrics_cache_misses{0};
	std::atomic<uint64_t> metrics_visible_builds{0};
	std::atomic<uint64_t> metrics_prefetch_requests{0};
	std::atomic<uint64_t> metrics_prefetch_builds{0};
	std::atomic<uint64_t> metrics_stale_drops{0};
	std::atomic<uint64_t> metrics_evictions{0};

	void RecreateCache();
	std::unique_ptr<float[]> BuildBlock(size_t block_index);
	void TouchLocked(size_t block_index);
	void TrimLocked();
	void DrainReady();
	void ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation);

public:
	AudioSpectrumAnalysisCache();
	~AudioSpectrumAnalysisCache();

	void SetSource(AudioDisplaySource *new_source);
	void SetMixPolicy(AudioMixPolicy new_policy);
	void SetResolution(size_t new_derivation_size, size_t new_derivation_dist);
	void Age(size_t max_size);
	bool IsReady() const;
	const float* Get(size_t block_index);
	void Prefetch(size_t first_block, size_t last_block);
	AudioSpectrumAnalysisCacheMetrics GetMetricsSnapshot() const;
};
