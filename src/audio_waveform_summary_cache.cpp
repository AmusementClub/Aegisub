#include "audio_waveform_summary_cache.h"

#include "audio_latest_range_scheduler.h"
#include "audio_waveform_column_ref.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr size_t block_bytes = sizeof(AudioWaveformSummaryBlock);
constexpr size_t kWaveformBuildChunkTargetBytes = 8 * 1024 * 1024;
constexpr size_t kWaveformBuildProviderChunkMaxMilliseconds = 2000;
constexpr size_t kWaveformPrefetchHardMaxBlocks = 64;

struct WaveformSummaryScratch {
	std::vector<float> float_samples;
	std::vector<int16_t> pcm16_samples;
};

WaveformSummaryScratch& GetWaveformSummaryScratch() {
	thread_local WaveformSummaryScratch scratch;
	return scratch;
}

void AnalyzePcm16Mono(
	int16_t const* samples,
	int64_t sample_count,
	AudioWaveformSummary &summary,
	AudioWaveformPcm16Summary &pcm16_summary) {
	if (!samples || sample_count <= 0)
		return;

	for (int64_t i = 0; i < sample_count; ++i) {
		const int sample = samples[i];
		if (sample > 0) {
			pcm16_summary.peak_max = std::max(pcm16_summary.peak_max, sample);
			pcm16_summary.avg_max_accum += sample;
		}
		else {
			pcm16_summary.peak_min = std::min(pcm16_summary.peak_min, sample);
			pcm16_summary.avg_min_accum += sample;
		}
	}

	constexpr double pcm_scale = 1.0 / 32768.0;
	summary.peak_min = static_cast<float>(pcm16_summary.peak_min * pcm_scale);
	summary.peak_max = static_cast<float>(pcm16_summary.peak_max * pcm_scale);
	summary.avg_min = static_cast<float>(pcm16_summary.avg_min_accum * pcm_scale / sample_count);
	summary.avg_max = static_cast<float>(pcm16_summary.avg_max_accum * pcm_scale / sample_count);
}
}

AudioWaveformSummaryCache::AudioWaveformSummaryCache() = default;

AudioWaveformSummaryCache::~AudioWaveformSummaryCache() {
	StopScheduler();
}

void AudioWaveformSummaryCache::StopScheduler() {
	std::unique_ptr<AudioLatestRangeScheduler> old_scheduler;
	{
		std::lock_guard<std::mutex> lock(scheduler_mutex);
		active_prefetch_generation.store(0, std::memory_order_release);
		has_active_prefetch_range = false;
		if (scheduler)
			scheduler->Invalidate();
		old_scheduler = std::move(scheduler);
	}
}

bool AudioWaveformSummaryCache::IsCurrentPrefetchGeneration(uint64_t generation) const {
	return generation != 0
		&& generation == active_prefetch_generation.load(std::memory_order_acquire);
}

void AudioWaveformSummaryCache::NotifyReady() const {
	std::function<void()> callback;
	{
		std::lock_guard<std::mutex> lock(ready_callback_mutex);
		callback = ready_callback;
	}
	if (!callback)
		return;
	try {
		callback();
	}
	catch (...) {
	}
}

void AudioWaveformSummaryCache::ClearLocked() {
	cache_blocks.clear();
	cache_touch.clear();
	touch_heap = {};
	current_cache_bytes = 0;
	current_cache_entries = 0;
	touch_counter = 0;
}

void AudioWaveformSummaryCache::RecreateCache() {
	ClearLocked();
	block_count = 0;
	if (source && pixel_ms > 0.0 && source->GetSampleRate() > 0 && source->GetNumSamples() > 0) {
		const double duration = source->GetNumSamples() * 1000.0 / source->GetSampleRate();
		const double blocks = duration / pixel_ms / AudioWaveformSummaryBlock::width;
		const double size_t_exclusive_upper = std::ldexp(
			1.0,
			std::numeric_limits<size_t>::digits);
		if (std::isfinite(blocks) && blocks >= 0.0
			&& blocks < size_t_exclusive_upper) {
			const size_t new_block_count = std::max<size_t>(1, static_cast<size_t>(blocks));
			if (new_block_count <= cache_blocks.max_size()
				&& new_block_count <= cache_touch.max_size()) {
				block_count = new_block_count;
				cache_blocks.resize(block_count);
				cache_touch.resize(block_count);
			}
		}
	}
	++metrics_generation;
}

AudioWaveformSummaryCache::BuiltBlocks AudioWaveformSummaryCache::BuildBlocks(
	size_t first_block,
	size_t last_block,
	uint64_t generation) const {
	BuiltBlocks result;
	if (first_block > last_block || !source || pixel_ms <= 0.0
		|| source->GetSampleRate() <= 0 || block_count == 0 || first_block >= block_count) {
		return result;
	}

	last_block = std::min(last_block, block_count - 1);
	const double samples_per_pixel = pixel_ms * source->GetSampleRate() / 1000.0;
	const double int64_exclusive_upper
		= -static_cast<double>(std::numeric_limits<int64_t>::min());
	if (!std::isfinite(samples_per_pixel) || samples_per_pixel <= 0.0
		|| samples_per_pixel >= int64_exclusive_upper) {
		return result;
	}
	const int64_t samples_per_column = static_cast<int64_t>(samples_per_pixel);
	const size_t channels = static_cast<size_t>(std::max(1, source->GetChannels()));
	const size_t scratch_bytes_per_frame = std::max(sizeof(int16_t), channels * sizeof(float));
	const uint64_t provider_chunk_frames_exact = static_cast<uint64_t>(source->GetSampleRate())
		* kWaveformBuildProviderChunkMaxMilliseconds / 1000;
	const size_t provider_chunk_frames = static_cast<size_t>(std::min<uint64_t>(
		provider_chunk_frames_exact,
		std::numeric_limits<size_t>::max()));
	const size_t max_chunk_frames = std::max<size_t>(1, std::min(
		provider_chunk_frames,
		kWaveformBuildChunkTargetBytes / scratch_bytes_per_frame));
	if (last_block > std::numeric_limits<size_t>::max() / AudioWaveformSummaryBlock::width)
		return result;

	auto &scratch = GetWaveformSummaryScratch();
	result.reserve(last_block - first_block + 1);
	for (size_t block_index = first_block; block_index <= last_block; ++block_index) {
		if (generation != 0 && !IsCurrentPrefetchGeneration(generation))
			break;

		auto block = std::make_shared<AudioWaveformSummaryBlock>();
		if (samples_per_column == 0) {
			result.emplace_back(block_index, std::move(block));
			continue;
		}
		const size_t block_first_pixel = block_index * AudioWaveformSummaryBlock::width;
		std::array<int64_t, AudioWaveformSummaryBlock::width> column_starts;
		for (size_t column = 0; column < column_starts.size(); ++column) {
			if (!GetWaveformColumnSampleStart(
				block_first_pixel + column,
				samples_per_pixel,
				column_starts[column])
				|| column_starts[column] > std::numeric_limits<int64_t>::max() - samples_per_column) {
				return {};
			}
		}

		bool exact_mode_known = false;
		bool exact_pcm16 = false;
		for (size_t chunk_first = 0; chunk_first < column_starts.size();) {
			if (generation != 0) {
				if (!IsCurrentPrefetchGeneration(generation)
					|| visible_waiters.load(std::memory_order_acquire) != 0) {
					return result;
				}
			}

			size_t chunk_last = chunk_first;
			const int64_t chunk_start = column_starts[chunk_first];
			int64_t chunk_end = chunk_start + samples_per_column;
			while (chunk_last + 1 < column_starts.size()) {
				const int64_t candidate_end = column_starts[chunk_last + 1] + samples_per_column;
				if (candidate_end < chunk_start)
					return {};
				const uint64_t candidate_frames = static_cast<uint64_t>(candidate_end - chunk_start);
				if (candidate_frames > max_chunk_frames)
					break;
				++chunk_last;
				chunk_end = candidate_end;
			}

			const int64_t chunk_frames = chunk_end - chunk_start;
			if (chunk_frames < 0
				|| static_cast<uint64_t>(chunk_frames) > std::numeric_limits<size_t>::max() / channels) {
				return {};
			}

			bool chunk_exact_pcm16 = false;
			if (chunk_frames > 0 && channels == 1) {
				scratch.pcm16_samples.resize(static_cast<size_t>(chunk_frames));
				chunk_exact_pcm16 = source->GetInt16MonoAudio(
					scratch.pcm16_samples.data(), chunk_start, chunk_frames);
			}
			if (!exact_mode_known) {
				exact_pcm16 = chunk_exact_pcm16;
				exact_mode_known = true;
			}
			else if (exact_pcm16 != chunk_exact_pcm16) {
				return {};
			}

			if (!chunk_exact_pcm16 && chunk_frames > 0) {
				const size_t interleaved_count = static_cast<size_t>(chunk_frames) * channels;
				scratch.float_samples.resize(interleaved_count);
				source->GetFloatAudio(scratch.float_samples.data(), chunk_start, chunk_frames);
			}

			for (size_t column = chunk_first; column <= chunk_last; ++column) {
				const int64_t offset_frames = column_starts[column] - chunk_start;
				if (offset_frames < 0 || offset_frames > chunk_frames
					|| samples_per_column > chunk_frames - offset_frames) {
					return {};
				}
				if (chunk_exact_pcm16) {
					AnalyzePcm16Mono(
						scratch.pcm16_samples.data() + static_cast<size_t>(offset_frames),
						samples_per_column,
						block->summaries[column],
						block->pcm16_summaries[column]);
				}
				else if (samples_per_column > 0) {
					const float *samples = scratch.float_samples.data()
						+ static_cast<size_t>(offset_frames) * channels;
					block->summaries[column] = AnalyzeWaveformInterleaved(
						samples,
						static_cast<int>(std::min<int64_t>(
							samples_per_column,
							std::numeric_limits<int>::max())),
						static_cast<int>(channels),
						mix_policy);
				}
			}

			chunk_first = chunk_last + 1;
		}
		block->has_exact_pcm16 = exact_mode_known && exact_pcm16;
		result.emplace_back(block_index, std::move(block));
	}

	return result;
}

void AudioWaveformSummaryCache::TouchLocked(size_t block_index) {
	const uint64_t touch = ++touch_counter;
	cache_touch[block_index] = touch;
	touch_heap.push(TouchEntry { touch, block_index });
	if (touch_heap.size() > 1024 && current_cache_entries < touch_heap.size() / 4)
		CompactTouchHeapLocked();
}

void AudioWaveformSummaryCache::CompactTouchHeapLocked() {
	decltype(touch_heap) compacted;
	while (!touch_heap.empty()) {
		const auto candidate = touch_heap.top();
		touch_heap.pop();
		if (candidate.index < cache_blocks.size()
			&& cache_blocks[candidate.index]
			&& cache_touch[candidate.index] == candidate.touch) {
			compacted.push(candidate);
		}
	}
	touch_heap = std::move(compacted);
}

void AudioWaveformSummaryCache::TrimLocked() {
	while (current_cache_bytes > max_cache_bytes && current_cache_entries > 0) {
		size_t victim = block_count;
		while (!touch_heap.empty()) {
			const auto candidate = touch_heap.top();
			touch_heap.pop();
			if (candidate.index >= cache_blocks.size() || !cache_blocks[candidate.index])
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
		--current_cache_entries;
		++metrics_evictions;
	}
}

void AudioWaveformSummaryCache::SetSource(AudioDisplaySource *new_source) {
	StopScheduler();
	std::lock_guard<std::mutex> build_lock(build_mutex);
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (source == new_source)
		return;
	source = new_source;
	RecreateCache();
}

void AudioWaveformSummaryCache::SetMillisecondsPerPixel(double new_pixel_ms) {
	StopScheduler();
	std::lock_guard<std::mutex> build_lock(build_mutex);
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (pixel_ms == new_pixel_ms)
		return;
	pixel_ms = new_pixel_ms;
	RecreateCache();
}

void AudioWaveformSummaryCache::SetMixPolicy(AudioMixPolicy new_policy) {
	StopScheduler();
	std::lock_guard<std::mutex> build_lock(build_mutex);
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (mix_policy == new_policy)
		return;
	mix_policy = new_policy;
	RecreateCache();
}

void AudioWaveformSummaryCache::Age(size_t max_size) {
	if (max_size == 0) {
		StopScheduler();
		std::lock_guard<std::mutex> build_lock(build_mutex);
		std::lock_guard<std::mutex> lock(cache_mutex);
		ClearLocked();
		cache_blocks.resize(block_count);
		cache_touch.resize(block_count);
		return;
	}

	std::lock_guard<std::mutex> lock(cache_mutex);
	max_cache_bytes = std::max(block_bytes, max_size);
	TrimLocked();
}

bool AudioWaveformSummaryCache::IsReady() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return source && pixel_ms > 0.0 && block_count > 0;
}

AudioWaveformSummaryCache::BlockHandle AudioWaveformSummaryCache::Get(size_t block_index) {
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index >= block_count)
			return {};
		if (cache_blocks[block_index]) {
			TouchLocked(block_index);
			++metrics_cache_hits;
			return cache_blocks[block_index];
		}
	}

	std::unique_lock<std::mutex> build_lock(build_mutex, std::try_to_lock);
	if (!build_lock.owns_lock()) {
		visible_waiters.fetch_add(1, std::memory_order_acq_rel);
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			++metrics_visible_lock_contention;
		}
		try {
			build_lock.lock();
		}
		catch (...) {
			visible_waiters.fetch_sub(1, std::memory_order_acq_rel);
			throw;
		}
		visible_waiters.fetch_sub(1, std::memory_order_acq_rel);
	}

	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index >= block_count)
			return {};
		if (cache_blocks[block_index]) {
			TouchLocked(block_index);
			++metrics_cache_hits;
			return cache_blocks[block_index];
		}
	}

	auto built_blocks = BuildBlocks(block_index, block_index);
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_index >= block_count)
		return {};
	++metrics_cache_misses;
	if (!built_blocks.empty())
		++metrics_visible_builds;
	for (auto &pair : built_blocks) {
		const size_t index = pair.first;
		if (index >= cache_blocks.size())
			continue;
		if (!cache_blocks[index]) {
			cache_blocks[index] = std::move(pair.second);
			current_cache_bytes += block_bytes;
			++current_cache_entries;
		}
		TouchLocked(index);
	}
	TrimLocked();
	return cache_blocks[block_index];
}

AudioWaveformSummaryCache::BlockHandle AudioWaveformSummaryCache::GetIfReady(size_t block_index) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_index >= cache_blocks.size() || !cache_blocks[block_index])
		return {};
	return cache_blocks[block_index];
}

bool AudioWaveformSummaryCache::AreBlocksReady(size_t first_block, size_t last_block) {
	if (last_block < first_block)
		return true;
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_count == 0 || first_block >= block_count)
		return false;
	last_block = std::min(last_block, block_count - 1);
	for (size_t block_index = first_block; block_index <= last_block; ++block_index) {
		if (!cache_blocks[block_index])
			return false;
	}
	return true;
}

void AudioWaveformSummaryCache::Prefetch(size_t first_block, size_t last_block) {
	if (!prefetch_enabled.load(std::memory_order_relaxed) || last_block < first_block)
		return;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (!source || block_count == 0 || first_block >= block_count)
			return;
		last_block = std::min(last_block, block_count - 1);
		++metrics_prefetch_requests;
	}

	std::lock_guard<std::mutex> lock(scheduler_mutex);
	if (has_active_prefetch_range
		&& active_prefetch_first == first_block
		&& active_prefetch_last == last_block) {
		return;
	}
	if (!scheduler) {
		scheduler = std::make_unique<AudioLatestRangeScheduler>(
			[this](size_t first, size_t last, uint64_t generation) {
				ProcessPrefetch(first, last, generation);
			});
	}
	const uint64_t generation = scheduler->CurrentGeneration() + 1;
	active_prefetch_generation.store(generation, std::memory_order_release);
	has_active_prefetch_range = true;
	active_prefetch_first = first_block;
	active_prefetch_last = last_block;
	scheduler->Request(first_block, last_block);
}

void AudioWaveformSummaryCache::SetPrefetchEnabled(bool enabled) {
	prefetch_enabled.store(enabled, std::memory_order_relaxed);
	if (!enabled)
		StopScheduler();
}

void AudioWaveformSummaryCache::SetPrefetchBuildMaxBlocks(size_t max_blocks) {
	prefetch_build_max_blocks.store(
		std::max<size_t>(1, std::min(kWaveformPrefetchHardMaxBlocks, max_blocks)),
		std::memory_order_relaxed);
}

void AudioWaveformSummaryCache::SetReadyCallback(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(ready_callback_mutex);
	ready_callback = std::move(callback);
}

void AudioWaveformSummaryCache::ProcessPrefetch(
	size_t first_block,
	size_t last_block,
	uint64_t generation) {
	bool built_any = false;
	bool stale_recorded = false;
	auto record_stale = [&] {
		if (stale_recorded)
			return;
		std::lock_guard<std::mutex> lock(cache_mutex);
		++metrics_stale_drops;
		stale_recorded = true;
	};
	auto record_busy = [&] {
		std::lock_guard<std::mutex> lock(cache_mutex);
		++metrics_prefetch_busy_skips;
	};

	auto build_one = [&](size_t block_index) {
		if (!IsCurrentPrefetchGeneration(generation)) {
			record_stale();
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			if (block_index >= block_count)
				return false;
			if (cache_blocks[block_index])
				return true;
		}
		if (visible_waiters.load(std::memory_order_acquire) != 0) {
			record_busy();
			return false;
		}

		std::unique_lock<std::mutex> build_lock(build_mutex, std::try_to_lock);
		if (!build_lock.owns_lock()) {
			record_busy();
			return false;
		}
		if (!IsCurrentPrefetchGeneration(generation)) {
			record_stale();
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			if (block_index >= block_count)
				return false;
			if (cache_blocks[block_index])
				return true;
		}

		BuiltBlocks built_blocks;
		try {
			built_blocks = BuildBlocks(block_index, block_index, generation);
		}
		catch (...) {
			record_busy();
			return false;
		}
		if (built_blocks.empty()) {
			if (!IsCurrentPrefetchGeneration(generation))
				record_stale();
			else if (visible_waiters.load(std::memory_order_acquire) != 0)
				record_busy();
			return false;
		}

		{
			std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex);
			if (!IsCurrentPrefetchGeneration(generation)) {
				record_stale();
				return false;
			}

			std::lock_guard<std::mutex> lock(cache_mutex);
			if (block_index >= block_count)
				return false;
			if (!cache_blocks[block_index]) {
				cache_blocks[block_index] = std::move(built_blocks.front().second);
				current_cache_bytes += block_bytes;
				++current_cache_entries;
				++metrics_prefetch_builds;
				built_any = true;
			}
			TouchLocked(block_index);
			TrimLocked();
		}
		return true;
	};

	auto hint_and_build = [&](size_t range_first, size_t range_last) {
		if (range_last < range_first || !IsCurrentPrefetchGeneration(generation))
			return false;
		try {
			const long double samples_per_pixel = static_cast<long double>(pixel_ms)
				* source->GetSampleRate() / 1000.0L;
			const long double first_sample = static_cast<long double>(range_first)
				* AudioWaveformSummaryBlock::width * samples_per_pixel;
			const long double last_sample = ((static_cast<long double>(range_last) + 1.0L)
				* AudioWaveformSummaryBlock::width * samples_per_pixel);
			if (std::isfinite(samples_per_pixel) && samples_per_pixel > 0.0L
				&& first_sample >= 0.0L && last_sample > first_sample
				&& last_sample <= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
				source->HintFloatAudio(
					static_cast<int64_t>(first_sample),
					static_cast<int64_t>(last_sample - first_sample));
			}
		}
		catch (...) {
			record_busy();
			return false;
		}
		for (size_t block_index = range_first; block_index <= range_last; ++block_index) {
			if (!build_one(block_index))
				return false;
		}
		return true;
	};

	const size_t max_blocks = prefetch_build_max_blocks.load(std::memory_order_relaxed);
	const size_t span_minus_one = last_block - first_block;
	if (span_minus_one < max_blocks) {
		hint_and_build(first_block, last_block);
	}
	else {
		const size_t lower_count = (max_blocks + 1) / 2;
		const size_t upper_count = max_blocks - lower_count;
		const bool lower_complete = hint_and_build(
			first_block,
			first_block + lower_count - 1);
		if (lower_complete && upper_count > 0)
			hint_and_build(last_block - upper_count + 1, last_block);
	}

	{
		std::lock_guard<std::mutex> lock(scheduler_mutex);
		if (IsCurrentPrefetchGeneration(generation)) {
			active_prefetch_generation.store(0, std::memory_order_release);
			has_active_prefetch_range = false;
		}
	}
	if (built_any)
		NotifyReady();
}

AudioWaveformSummaryCacheMetrics AudioWaveformSummaryCache::GetMetricsSnapshot() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	AudioWaveformSummaryCacheMetrics metrics;
	metrics.generation = metrics_generation;
	metrics.cache_hits = metrics_cache_hits;
	metrics.cache_misses = metrics_cache_misses;
	metrics.visible_builds = metrics_visible_builds;
	metrics.visible_lock_contention = metrics_visible_lock_contention;
	metrics.prefetch_requests = metrics_prefetch_requests;
	metrics.prefetch_builds = metrics_prefetch_builds;
	metrics.prefetch_busy_skips = metrics_prefetch_busy_skips;
	metrics.prefetch_enabled = prefetch_enabled.load(std::memory_order_relaxed);
	metrics.stale_drops = metrics_stale_drops;
	metrics.evictions = metrics_evictions;
	metrics.cache_entries = current_cache_entries;
	metrics.cache_bytes = current_cache_bytes;
	metrics.cache_budget_bytes = max_cache_bytes;
	metrics.cache_touch_entries = touch_heap.size();
	return metrics;
}
