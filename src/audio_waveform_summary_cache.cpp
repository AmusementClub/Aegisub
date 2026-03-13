#include "audio_waveform_summary_cache.h"

#include <algorithm>

namespace {
constexpr size_t block_bytes = sizeof(AudioWaveformSummaryBlock);
}

AudioWaveformSummaryCache::AudioWaveformSummaryCache() {
}

AudioWaveformSummaryCache::~AudioWaveformSummaryCache() = default;

void AudioWaveformSummaryCache::RecreateCache() {
	std::lock_guard<std::mutex> lock(cache_mutex);
	cache_blocks.clear();
	cache_touch.clear();
	current_cache_bytes = 0;
	touch_counter = 0;
	{
		std::lock_guard<std::mutex> ready_lock(ready_mutex);
		ready_blocks.clear();
		has_ready_blocks = false;
	}
	if (!source || pixel_ms <= 0.0 || source->GetSampleRate() <= 0 || source->GetNumSamples() <= 0) {
		block_count = 0;
		metrics_generation.fetch_add(1, std::memory_order_relaxed);
		if (scheduler)
			scheduler->Invalidate();
		return;
	}

	const double duration = source->GetNumSamples() * 1000.0 / source->GetSampleRate();
	block_count = static_cast<size_t>(duration / pixel_ms / AudioWaveformSummaryBlock::width);
	if (block_count == 0)
		block_count = 1;
	cache_blocks.resize(block_count);
	cache_touch.resize(block_count);
	metrics_generation.fetch_add(1, std::memory_order_relaxed);
	if (scheduler)
		scheduler->Invalidate();
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
	std::vector<float> audio_buffer(block_frames * channels);

	const int64_t block_start = static_cast<int64_t>(block_index * AudioWaveformSummaryBlock::width * pixel_samples);
	source->GetFloatAudio(audio_buffer.data(), block_start, static_cast<int64_t>(block_frames));

	const float *cur = audio_buffer.data();
	for (auto &summary : block->summaries) {
		summary = AnalyzeWaveformInterleaved(cur, samples_per_pixel, channels, mix_policy);
		cur += static_cast<size_t>(samples_per_pixel) * channels;
	}
	return block;
}

void AudioWaveformSummaryCache::TouchLocked(size_t block_index) {
	cache_touch[block_index] = ++touch_counter;
}

void AudioWaveformSummaryCache::TrimLocked() {
	while (current_cache_bytes > max_cache_bytes) {
		size_t victim = block_count;
		uint64_t oldest = UINT64_MAX;
		for (size_t i = 0; i < cache_blocks.size(); ++i) {
			if (!cache_blocks[i])
				continue;
			if (cache_touch[i] < oldest) {
				oldest = cache_touch[i];
				victim = i;
			}
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
		if (!cache_blocks[pair.first]) {
			cache_blocks[pair.first] = std::move(pair.second);
			current_cache_bytes += block_bytes;
			TouchLocked(pair.first);
			TrimLocked();
		}
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
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index < cache_blocks.size() && cache_blocks[block_index]) {
			TouchLocked(block_index);
			metrics_cache_hits.fetch_add(1, std::memory_order_relaxed);
			return *cache_blocks[block_index];
		}
	}

	metrics_cache_misses.fetch_add(1, std::memory_order_relaxed);
	auto built = BuildBlock(block_index);
	metrics_visible_builds.fetch_add(1, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_index < cache_blocks.size() && !cache_blocks[block_index]) {
		cache_blocks[block_index] = std::move(built);
		current_cache_bytes += block_bytes;
		TouchLocked(block_index);
		TrimLocked();
	}
	else if (block_index < cache_blocks.size()) {
		TouchLocked(block_index);
	}
	return *cache_blocks[block_index];
}

void AudioWaveformSummaryCache::Prefetch(size_t first_block, size_t last_block) {
	if (!IsReady() || last_block < first_block)
		return;
	if (!scheduler) {
		std::lock_guard<std::mutex> lock(scheduler_mutex);
		if (!scheduler) {
			scheduler = std::make_unique<AudioLatestRangeScheduler>([this](size_t first, size_t last, uint64_t generation) {
				ProcessPrefetch(first, last, generation);
			});
		}
	}
	metrics_prefetch_requests.fetch_add(last_block - first_block + 1, std::memory_order_relaxed);
	scheduler->Request(first_block, last_block);
}

void AudioWaveformSummaryCache::ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation) {
	for (size_t block_index = first_block; block_index <= last_block; ++block_index) {
		if (!scheduler->IsCurrent(generation)) {
			metrics_stale_drops.fetch_add(1, std::memory_order_relaxed);
			break;
		}

		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			if (block_index < cache_blocks.size() && cache_blocks[block_index])
				continue;
		}

		auto built = BuildBlock(block_index);
		if (!scheduler->IsCurrent(generation)) {
			metrics_stale_drops.fetch_add(1, std::memory_order_relaxed);
			break;
		}

		std::lock_guard<std::mutex> lock(ready_mutex);
		ready_blocks.emplace_back(block_index, std::move(built));
		has_ready_blocks = true;
		metrics_prefetch_builds.fetch_add(1, std::memory_order_relaxed);
	}
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
