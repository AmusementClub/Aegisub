#include "audio_waveform_summary_cache.h"

#include <algorithm>

namespace {
constexpr size_t block_bytes = sizeof(AudioWaveformSummaryBlock);
constexpr size_t kWaveformBuildBatchMaxTempBytes = 8 * 1024 * 1024;
constexpr size_t kWaveformSyncBuildMaxBlocks = 32;
constexpr size_t kWaveformPrefetchBuildMaxBlocks = 64;

std::vector<float>& GetWaveformSummaryScratch() {
	thread_local std::vector<float> scratch;
	return scratch;
}
}

AudioWaveformSummaryCache::AudioWaveformSummaryCache() {
}

AudioWaveformSummaryCache::~AudioWaveformSummaryCache() {
	StopScheduler();
}

void AudioWaveformSummaryCache::StopScheduler() {
	std::unique_ptr<AudioLatestRangeScheduler> old_scheduler;
	{
		std::lock_guard<std::mutex> lock(scheduler_mutex);
		old_scheduler = std::move(scheduler);
	}
	active_prefetch_generation.store(0, std::memory_order_release);
}

bool AudioWaveformSummaryCache::IsCurrentPrefetchGeneration(uint64_t generation) const {
	return generation == active_prefetch_generation.load(std::memory_order_acquire);
}

void AudioWaveformSummaryCache::RecreateCache() {
	StopScheduler();
	std::lock_guard<std::mutex> lock(cache_mutex);
	cache_blocks.clear();
	cache_touch.clear();
	touch_heap = {};
	current_cache_bytes = 0;
	touch_counter = 0;
	pending_blocks.clear();
	{
		std::lock_guard<std::mutex> ready_lock(ready_mutex);
		ready_blocks.clear();
		has_ready_blocks = false;
	}
	if (!source || pixel_ms <= 0.0 || source->GetSampleRate() <= 0 || source->GetNumSamples() <= 0) {
		block_count = 0;
		metrics_generation.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	const double duration = source->GetNumSamples() * 1000.0 / source->GetSampleRate();
	block_count = static_cast<size_t>(duration / pixel_ms / AudioWaveformSummaryBlock::width);
	if (block_count == 0)
		block_count = 1;
	cache_blocks.resize(block_count);
	cache_touch.resize(block_count);
	pending_blocks.resize(block_count);
	metrics_generation.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<AudioWaveformSummaryBlock> AudioWaveformSummaryCache::BuildBlock(size_t block_index) const {
	auto block = std::make_unique<AudioWaveformSummaryBlock>();
	if (!source || pixel_ms <= 0.0 || block_index >= block_count) {
		for (auto &summary : block->summaries)
			summary = AudioWaveformSummary();
		return block;
	}

	const int channels = std::max(1, source->GetChannels());
	const double pixel_samples = pixel_ms * source->GetSampleRate() / 1000.0;
	const int samples_per_pixel = std::max(1, static_cast<int>(pixel_samples));
	const size_t block_frames = AudioWaveformSummaryBlock::width * static_cast<size_t>(samples_per_pixel);
	auto &audio_buffer = GetWaveformSummaryScratch();
	audio_buffer.resize(block_frames * channels);

	const int64_t block_start = static_cast<int64_t>(block_index * AudioWaveformSummaryBlock::width * pixel_samples);
	source->GetFloatAudio(audio_buffer.data(), block_start, static_cast<int64_t>(block_frames));

	const float *cur = audio_buffer.data();
	for (auto &summary : block->summaries) {
		summary = AnalyzeWaveformInterleaved(cur, samples_per_pixel, channels, mix_policy);
		cur += static_cast<size_t>(samples_per_pixel) * channels;
	}
	return block;
}

size_t AudioWaveformSummaryCache::GetMaxBuildBlocks(size_t preferred_cap) const {
	if (!source || pixel_ms <= 0.0 || source->GetSampleRate() <= 0)
		return 1;

	const int channels = std::max(1, source->GetChannels());
	const double pixel_samples = pixel_ms * source->GetSampleRate() / 1000.0;
	const size_t samples_per_pixel = static_cast<size_t>(std::max(1, static_cast<int>(pixel_samples)));
	const size_t frames_per_block = AudioWaveformSummaryBlock::width * samples_per_pixel;
	const size_t bytes_per_block = frames_per_block * static_cast<size_t>(channels) * sizeof(float);
	if (bytes_per_block == 0 || bytes_per_block >= kWaveformBuildBatchMaxTempBytes)
		return 1;

	const size_t temp_limited = std::max<size_t>(1, kWaveformBuildBatchMaxTempBytes / bytes_per_block);
	return std::max<size_t>(1, std::min(preferred_cap, temp_limited));
}

std::vector<std::pair<size_t, std::unique_ptr<AudioWaveformSummaryBlock>>> AudioWaveformSummaryCache::BuildBlocks(size_t first_block, size_t last_block) const {
	std::vector<std::pair<size_t, std::unique_ptr<AudioWaveformSummaryBlock>>> result;
	if (first_block > last_block)
		return result;
	if (!source || pixel_ms <= 0.0 || source->GetSampleRate() <= 0 || block_count == 0)
		return result;

	last_block = std::min(last_block, block_count - 1);
	result.reserve(last_block - first_block + 1);

	const int channels = std::max(1, source->GetChannels());
	const double pixel_samples = pixel_ms * source->GetSampleRate() / 1000.0;
	const int samples_per_pixel = std::max(1, static_cast<int>(pixel_samples));
	const int64_t block_frames = static_cast<int64_t>(AudioWaveformSummaryBlock::width) * samples_per_pixel;
	const int64_t batch_start = static_cast<int64_t>(first_block * AudioWaveformSummaryBlock::width * pixel_samples);
	const int64_t last_block_start = static_cast<int64_t>(last_block * AudioWaveformSummaryBlock::width * pixel_samples);
	const int64_t batch_frames = std::max<int64_t>(block_frames, last_block_start + block_frames - batch_start);

	auto &audio_buffer = GetWaveformSummaryScratch();
	audio_buffer.resize(static_cast<size_t>(batch_frames) * channels);
	source->GetFloatAudio(audio_buffer.data(), batch_start, batch_frames);

	for (size_t block_index = first_block; block_index <= last_block; ++block_index) {
		auto block = std::make_unique<AudioWaveformSummaryBlock>();
		const int64_t block_start = static_cast<int64_t>(block_index * AudioWaveformSummaryBlock::width * pixel_samples);
		const size_t offset_frames = static_cast<size_t>(std::max<int64_t>(0, block_start - batch_start));
		const float *cur = audio_buffer.data() + offset_frames * channels;
		for (auto &summary : block->summaries) {
			summary = AnalyzeWaveformInterleaved(cur, samples_per_pixel, channels, mix_policy);
			cur += static_cast<size_t>(samples_per_pixel) * channels;
		}
		result.emplace_back(block_index, std::move(block));
	}

	return result;
}

void AudioWaveformSummaryCache::TouchLocked(size_t block_index) {
	const uint64_t touch = ++touch_counter;
	cache_touch[block_index] = touch;
	touch_heap.push(TouchEntry{ touch, block_index });
}

void AudioWaveformSummaryCache::TrimLocked() {
	while (current_cache_bytes > max_cache_bytes) {
		size_t victim = block_count;
		while (!touch_heap.empty()) {
			const auto candidate = touch_heap.top();
			touch_heap.pop();
			if (candidate.index >= cache_blocks.size())
				continue;
			if (!cache_blocks[candidate.index])
				continue;
			if (cache_touch[candidate.index] != candidate.touch)
				continue;
			victim = candidate.index;
			break;
		}
		if (victim == block_count)
			break;
		cache_blocks[victim].reset();
		cache_touch[victim] = 0;
		current_cache_bytes -= block_bytes;
		metrics_evictions.fetch_add(1, std::memory_order_relaxed);
	}
}

void AudioWaveformSummaryCache::DrainReady() {
	if (!has_ready_blocks.load(std::memory_order_acquire))
		return;

	std::vector<std::pair<size_t, std::unique_ptr<AudioWaveformSummaryBlock>>> ready;
	{
		std::lock_guard<std::mutex> lock(ready_mutex);
		if (ready_blocks.empty())
			return;
		ready.swap(ready_blocks);
		has_ready_blocks = false;
	}

	std::lock_guard<std::mutex> lock(cache_mutex);
	for (auto &pair : ready) {
		if (pair.first >= cache_blocks.size())
			continue;
		if (pair.first < pending_blocks.size())
			pending_blocks[pair.first] = 0;
		if (!cache_blocks[pair.first]) {
			cache_blocks[pair.first] = std::move(pair.second);
			current_cache_bytes += block_bytes;
			TouchLocked(pair.first);
			TrimLocked();
		}
	}
}

void AudioWaveformSummaryCache::ClearPendingRange(size_t first_block, size_t last_block) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (pending_blocks.empty() || first_block > last_block)
		return;
	last_block = std::min(last_block, pending_blocks.size() - 1);
	for (size_t i = first_block; i <= last_block; ++i) {
		if (i < cache_blocks.size() && !cache_blocks[i])
			pending_blocks[i] = 0;
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
	std::lock_guard<std::mutex> lock(cache_mutex);
	max_cache_bytes = std::max(block_bytes, max_size);
	TrimLocked();
}

bool AudioWaveformSummaryCache::IsReady() const {
	return source && pixel_ms > 0.0 && block_count > 0;
}

const AudioWaveformSummaryBlock& AudioWaveformSummaryCache::Get(size_t block_index) {
	DrainReady();
	size_t last_block_to_build = block_index;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index < cache_blocks.size() && cache_blocks[block_index]) {
			if (block_index < pending_blocks.size())
				pending_blocks[block_index] = 0;
			TouchLocked(block_index);
			metrics_cache_hits.fetch_add(1, std::memory_order_relaxed);
			return *cache_blocks[block_index];
		}

		if (block_index < cache_blocks.size()) {
			const size_t max_blocks = GetMaxBuildBlocks(kWaveformSyncBuildMaxBlocks);
			const size_t limit = std::min(cache_blocks.size(), block_index + max_blocks);
			while (last_block_to_build + 1 < limit && !cache_blocks[last_block_to_build + 1])
				++last_block_to_build;
		}
	}

	metrics_cache_misses.fetch_add(1, std::memory_order_relaxed);
	auto built_blocks = BuildBlocks(block_index, last_block_to_build);
	metrics_visible_builds.fetch_add(1, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(cache_mutex);
	for (auto &pair : built_blocks) {
		const size_t index = pair.first;
		if (index >= cache_blocks.size())
			continue;
		if (!cache_blocks[index]) {
			cache_blocks[index] = std::move(pair.second);
			current_cache_bytes += block_bytes;
		}
		if (index < pending_blocks.size())
			pending_blocks[index] = 0;
		TouchLocked(index);
	}
	if (!built_blocks.empty())
		TrimLocked();

	if (block_index < cache_blocks.size() && !cache_blocks[block_index]) {
		cache_blocks[block_index] = BuildBlock(block_index);
		current_cache_bytes += block_bytes;
		if (block_index < pending_blocks.size())
			pending_blocks[block_index] = 0;
		TouchLocked(block_index);
		TrimLocked();
	}
	return *cache_blocks[block_index];
}

const AudioWaveformSummaryBlock* AudioWaveformSummaryCache::GetIfReady(size_t block_index) {
	DrainReady();
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_index < cache_blocks.size())
		return cache_blocks[block_index].get();
	return nullptr;
}

bool AudioWaveformSummaryCache::AreBlocksReady(size_t first_block, size_t last_block) {
	if (last_block < first_block)
		return true;

	DrainReady();
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (cache_blocks.empty() || first_block >= cache_blocks.size())
		return false;

	last_block = std::min(last_block, cache_blocks.size() - 1);
	for (size_t block_index = first_block; block_index <= last_block; ++block_index) {
		if (!cache_blocks[block_index])
			return false;
	}
	return true;
}

void AudioWaveformSummaryCache::Prefetch(size_t first_block, size_t last_block) {
	if (!prefetch_enabled.load(std::memory_order_relaxed))
		return;
	if (!IsReady() || last_block < first_block)
		return;
	if (first_block >= block_count)
		return;
	last_block = std::min(last_block, block_count - 1);

	const double pixel_samples = pixel_ms * source->GetSampleRate() / 1000.0;
	const int samples_per_pixel = std::max(1, static_cast<int>(pixel_samples));
	const int64_t block_frames = static_cast<int64_t>(AudioWaveformSummaryBlock::width) * samples_per_pixel;
	const int64_t start_frame = static_cast<int64_t>(first_block * AudioWaveformSummaryBlock::width * pixel_samples);
	const int64_t end_frame = static_cast<int64_t>(last_block * AudioWaveformSummaryBlock::width * pixel_samples) + block_frames;

	bool should_request = false;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		for (size_t i = first_block; i <= last_block; ++i) {
			if (!cache_blocks[i] && !pending_blocks[i]) {
				pending_blocks[i] = 1;
				should_request = true;
			}
		}
		if (!should_request)
			return;
	}

	source->HintFloatAudio(start_frame, end_frame - start_frame);

	{
		std::lock_guard<std::mutex> lock(scheduler_mutex);
		if (!scheduler) {
			scheduler = std::make_unique<AudioLatestRangeScheduler>([this](size_t first, size_t last, uint64_t generation) {
				ProcessPrefetch(first, last, generation);
			});
		}
		active_prefetch_generation.store(scheduler->CurrentGeneration() + 1, std::memory_order_release);
		scheduler->Request(first_block, last_block);
	}
	metrics_prefetch_requests.fetch_add(last_block - first_block + 1, std::memory_order_relaxed);
}

void AudioWaveformSummaryCache::SetPrefetchEnabled(bool enabled) {
	prefetch_enabled.store(enabled, std::memory_order_relaxed);
	if (!enabled) {
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			std::fill(pending_blocks.begin(), pending_blocks.end(), uint8_t{0});
		}
		StopScheduler();
	}
}

void AudioWaveformSummaryCache::ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation) {
	bool enqueued_ready = false;
	size_t block_index = first_block;
	while (block_index <= last_block) {
		if (!IsCurrentPrefetchGeneration(generation)) {
			metrics_stale_drops.fetch_add(1, std::memory_order_relaxed);
			ClearPendingRange(block_index, last_block);
			break;
		}

		size_t chunk_first = block_index;
		size_t chunk_last = block_index;
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			while (chunk_first <= last_block && chunk_first < cache_blocks.size() && cache_blocks[chunk_first]) {
				if (chunk_first < pending_blocks.size())
					pending_blocks[chunk_first] = 0;
				++chunk_first;
			}
			if (chunk_first > last_block || chunk_first >= cache_blocks.size())
				break;

			chunk_last = chunk_first;
			const size_t max_blocks = GetMaxBuildBlocks(kWaveformPrefetchBuildMaxBlocks);
			const size_t limit = std::min(cache_blocks.size(), chunk_first + max_blocks);
			while (chunk_last + 1 <= last_block && chunk_last + 1 < limit && !cache_blocks[chunk_last + 1])
				++chunk_last;
		}

		auto built_blocks = BuildBlocks(chunk_first, chunk_last);
		if (!IsCurrentPrefetchGeneration(generation)) {
			metrics_stale_drops.fetch_add(1, std::memory_order_relaxed);
			ClearPendingRange(chunk_first, last_block);
			break;
		}

		{
			std::lock_guard<std::mutex> lock(ready_mutex);
			for (auto &pair : built_blocks)
				ready_blocks.emplace_back(pair.first, std::move(pair.second));
		}
		if (!built_blocks.empty()) {
			has_ready_blocks = true;
			metrics_prefetch_builds.fetch_add(built_blocks.size(), std::memory_order_relaxed);
			enqueued_ready = true;
		}
		block_index = chunk_last + 1;
	}

	if (enqueued_ready && ready_callback)
		ready_callback();
}

AudioWaveformSummaryCacheMetrics AudioWaveformSummaryCache::GetMetricsSnapshot() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	AudioWaveformSummaryCacheMetrics m;
	m.generation = metrics_generation.load(std::memory_order_relaxed);
	m.cache_hits = metrics_cache_hits.load(std::memory_order_relaxed);
	m.cache_misses = metrics_cache_misses.load(std::memory_order_relaxed);
	m.visible_builds = metrics_visible_builds.load(std::memory_order_relaxed);
	m.prefetch_requests = metrics_prefetch_requests.load(std::memory_order_relaxed);
	m.prefetch_builds = metrics_prefetch_builds.load(std::memory_order_relaxed);
	m.stale_drops = metrics_stale_drops.load(std::memory_order_relaxed);
	m.evictions = metrics_evictions.load(std::memory_order_relaxed);
	m.cache_entries = std::count_if(cache_blocks.begin(), cache_blocks.end(), [](auto const& block) { return !!block; });
	m.cache_bytes = current_cache_bytes;
	return m;
}
