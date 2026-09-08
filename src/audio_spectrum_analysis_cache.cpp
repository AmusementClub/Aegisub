#include "audio_spectrum_analysis_cache.h"

#include "audio_latest_range_scheduler.h"

#ifdef WITH_FFTW3
#include "audio_spectrum_fftw3.h"
#endif

#include "fft.h"

#include <libaegisub/log.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr size_t kSpectrumPrefetchHardMaxBlocks = 64;

void FillBlockWithBuiltinFft(
	std::vector<float> &fft_scratch,
	std::vector<float> const& mono_scratch,
	float *block,
	size_t sample_count,
	size_t bin_count)
{
	float *fft_input = fft_scratch.data();
	float *fft_real = fft_scratch.data() + sample_count;
	float *fft_imag = fft_scratch.data() + sample_count * 2;
	std::copy(mono_scratch.begin(), mono_scratch.end(), fft_input);

	FFT fft;
	fft.Transform(sample_count, fft_input, fft_real, fft_imag);

	const float scale_factor = 9 / std::sqrt(2 * static_cast<float>(sample_count));
	for (size_t i = 0; i < bin_count; ++i) {
		float power = std::sqrt(fft_real[i] * fft_real[i] + fft_imag[i] * fft_imag[i]) * scale_factor;
		block[i] = std::log10(power + 1.f);
	}
}

}

AudioSpectrumAnalysisCache::AudioSpectrumAnalysisCache() {
}

AudioSpectrumAnalysisCache::~AudioSpectrumAnalysisCache() {
	StopScheduler();
	DestroyFftResources();
}

void AudioSpectrumAnalysisCache::StopScheduler() {
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

void AudioSpectrumAnalysisCache::DestroyFftResources() {
#ifdef WITH_FFTW3
	fftw3_transform.reset();
#endif

#ifdef WITH_PFFFT
	if (pffft_setup)
		pffft_destroy_setup(pffft_setup);
	pffft_setup = nullptr;

	if (pffft_input)
		pffft_aligned_free(pffft_input);
	if (pffft_output)
		pffft_aligned_free(pffft_output);
	if (pffft_work)
		pffft_aligned_free(pffft_work);
	pffft_input = nullptr;
	pffft_output = nullptr;
	pffft_work = nullptr;
#endif
	fft_scratch.clear();
}

void AudioSpectrumAnalysisCache::ClearLocked() {
	cache_blocks.clear();
	cache_touch.clear();
	silent_rebuild_blocks.clear();
	touch_heap = {};
	current_cache_bytes = 0;
	current_cache_entries = 0;
	touch_counter = 0;
	rolling_window_valid = false;
}

void AudioSpectrumAnalysisCache::RecreateCache() {
	std::lock_guard<std::mutex> lock(cache_mutex);
	ClearLocked();
	// Keep allocation size with the cache metadata so eviction does not need
	// the FFT build lock while a provider read is in flight.
	cache_block_bytes = sizeof(float) * BinCount();
	DestroyFftResources();

	if (!source || source->GetSampleRate() <= 0 || source->GetNumSamples() <= 0 || derivation_size == 0) {
		block_count = 0;
		++metrics_generation;
		return;
	}

	block_count = static_cast<size_t>((source->GetNumSamples() + HopSampleCount() - 1) >> derivation_dist);
	if (block_count == 0)
		block_count = 1;
	max_cache_bytes = std::max(max_cache_bytes, BlockBytes());

#ifdef WITH_FFTW3
	fftw3_transform = audio::spectrum::TryCreateFftw3SpectrumTransform(WindowSampleCount());
	const bool has_fftw_plan = !!fftw3_transform;
#else
	const bool has_fftw_plan = false;
#endif

	if (has_fftw_plan) {
		LOG_I("audio/spectrum/fft") << "Using FFTW3 backend";
	} else {
#ifdef WITH_PFFFT
		const size_t pffft_bytes = WindowSampleCount() * sizeof(float);
		pffft_setup = pffft_new_setup(static_cast<int>(WindowSampleCount()), PFFFT_REAL);
		if (pffft_setup) {
			pffft_input = static_cast<float *>(pffft_aligned_malloc(pffft_bytes));
			pffft_output = static_cast<float *>(pffft_aligned_malloc(pffft_bytes));
			pffft_work = static_cast<float *>(pffft_aligned_malloc(pffft_bytes));
			if (!pffft_input || !pffft_output || !pffft_work)
				DestroyFftResources();
		}
		if (pffft_setup) {
			LOG_I("audio/spectrum/fft") << "Using PFFFT backend";
		} else {
			LOG_I("audio/spectrum/fft") << "Using built-in FFT backend";
			fft_scratch.resize(WindowSampleCount() * 3);
		}
#else
		LOG_I("audio/spectrum/fft") << "Using built-in FFT backend";
		fft_scratch.resize(WindowSampleCount() * 3);
#endif
	}

#ifdef WITH_FFTW3
	if (!has_fftw_plan) {
		auto error = audio::spectrum::GetFftw3LoadError();
		if (!error.empty())
			LOG_D("audio/spectrum/fft") << "FFTW3 not available: " << error;
	}
#endif

	++metrics_generation;
}

void AudioSpectrumAnalysisCache::SetSource(AudioDisplaySource *new_source) {
	StopScheduler();
	std::lock_guard<std::mutex> build_lock(build_mutex);
	if (source == new_source)
		return;
	source = new_source;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::SetMixPolicy(AudioMixPolicy new_policy) {
	StopScheduler();
	std::lock_guard<std::mutex> build_lock(build_mutex);
	if (mix_policy == new_policy)
		return;
	mix_policy = new_policy;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::SetCacheFormat(SpectrumCacheFormat) {
	// Phase 3 intentionally fixes the production cache to Float32. The option
	// remains accepted for config compatibility, but it does not select a
	// second storage path until cache precision is re-evaluated with data.
	cache_format = SpectrumCacheFormat::Float32;
}

void AudioSpectrumAnalysisCache::SetResolution(size_t new_derivation_size, size_t new_derivation_dist) {
	StopScheduler();
	std::lock_guard<std::mutex> build_lock(build_mutex);
	new_derivation_dist = std::min(new_derivation_dist, new_derivation_size);
	if (derivation_size == new_derivation_size && derivation_dist == new_derivation_dist)
		return;
	derivation_size = new_derivation_size;
	derivation_dist = new_derivation_dist;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::Age(size_t max_size) {
	if (max_size == 0) {
		StopScheduler();
		std::scoped_lock lock(build_mutex, cache_mutex);
		ClearLocked();
		return;
	}
	std::scoped_lock lock(cache_mutex);
	max_cache_bytes = std::max(BlockBytes(), max_size);
	TrimLocked();
}

bool AudioSpectrumAnalysisCache::IsReady() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return source && block_count > 0 && derivation_size > 0;
}

AudioSpectrumAnalysisCache::MutableBlock AudioSpectrumAnalysisCache::BuildBlock(size_t block_index) {
	const int channels = std::max(1, source->GetChannels());
	const size_t sample_count = WindowSampleCount();
	const size_t hop_samples = HopSampleCount();
	const size_t bin_count = BinCount();
	auto block = MutableBlock(new float[bin_count], std::default_delete<float[]>());

	int64_t first_sample = (static_cast<int64_t>(block_index) << derivation_dist) - (static_cast<int64_t>(1) << derivation_size);
	bool reused_window = rolling_window_valid
		&& block_index == rolling_window_block_index + 1
		&& hop_samples <= sample_count
		&& mono_scratch.size() == sample_count;

	if (reused_window) {
		const size_t overlap_samples = sample_count - hop_samples;
		std::move(mono_scratch.begin() + hop_samples, mono_scratch.end(), mono_scratch.begin());
		audio_scratch.resize(hop_samples * channels);
		source->GetFloatAudio(audio_scratch.data(), first_sample + static_cast<int64_t>(overlap_samples), static_cast<int64_t>(hop_samples));
		float *tail = mono_scratch.data() + overlap_samples;
		if (channels == 1)
			std::copy(audio_scratch.begin(), audio_scratch.begin() + hop_samples, tail);
		else
			MixAudioToMono(mix_policy, audio_scratch.data(), static_cast<int>(hop_samples), channels, tail);
	}
	else {
		audio_scratch.resize(sample_count * channels);
		mono_scratch.resize(sample_count);
		source->GetFloatAudio(audio_scratch.data(), first_sample, static_cast<int64_t>(sample_count));
		if (channels == 1)
			std::copy(audio_scratch.begin(), audio_scratch.begin() + sample_count, mono_scratch.begin());
		else
			MixAudioToMono(mix_policy, audio_scratch.data(), static_cast<int>(sample_count), channels, mono_scratch.data());
	}

	rolling_window_valid = true;
	rolling_window_block_index = block_index;

#ifdef WITH_FFTW3
	if (fftw3_transform) {
		double scale_factor = 9 / std::sqrt(2 << (derivation_size + 1));
		if (fftw3_transform->Execute(mono_scratch.data(), block.get(), bin_count, scale_factor))
			return block;
		fftw3_transform.reset();
#ifdef WITH_PFFFT
		if (!pffft_setup)
			fft_scratch.resize(WindowSampleCount() * 3);
#else
		fft_scratch.resize(WindowSampleCount() * 3);
#endif
	}
#endif

#ifdef WITH_PFFFT
	if (pffft_setup) {
		std::copy(mono_scratch.begin(), mono_scratch.end(), pffft_input);
		pffft_transform_ordered(pffft_setup, pffft_input, pffft_output, pffft_work, PFFFT_FORWARD);

		const float scale_factor = 9 / std::sqrt(2 * static_cast<float>(sample_count));
		block[0] = std::log10(std::abs(pffft_output[0]) * scale_factor + 1.f);
		for (size_t i = 1; i < bin_count; ++i) {
			const float real = pffft_output[i * 2];
			const float imag = pffft_output[i * 2 + 1];
			const float power = std::sqrt(real * real + imag * imag) * scale_factor;
			block[i] = std::log10(power + 1.f);
		}
		return block;
	}
#endif

	FillBlockWithBuiltinFft(fft_scratch, mono_scratch, block.get(), sample_count, bin_count);
	return block;
}

void AudioSpectrumAnalysisCache::TouchLocked(size_t block_index) {
	const uint64_t touch = ++touch_counter;
	cache_touch[block_index] = touch;
	touch_heap.push(TouchEntry{ touch, block_index });
}

// A block of exact zeros is either genuine digital silence or the residue of
// a transient upstream read failure that was zero-filled. The two cannot be
// told apart here, so the first all-zero build of a block is handed out but
// not retained, giving a later rebuild the chance to recover real audio. A
// second all-zero build in the same generation is treated as genuine silence
// and cached so silent audio is not re-analyzed forever.
bool AudioSpectrumAnalysisCache::ShouldDeferSilentBlockLocked(
	size_t block_index,
	float const *block) {
	for (size_t i = 0, bin_count = BinCount(); i < bin_count; ++i) {
		if (block[i] != 0.f)
			return false;
	}
	if (silent_rebuild_blocks.erase(block_index) > 0)
		return false;
	silent_rebuild_blocks.insert(block_index);
	return true;
}

void AudioSpectrumAnalysisCache::TrimLocked() {
	const size_t block_bytes = BlockBytes();
	while (current_cache_bytes > max_cache_bytes && current_cache_entries > 0) {
		size_t victim = block_count;
		while (!touch_heap.empty()) {
			const auto candidate = touch_heap.top();
			touch_heap.pop();
			auto block_it = cache_blocks.find(candidate.index);
			if (block_it == cache_blocks.end() || !block_it->second)
				continue;
			auto touch_it = cache_touch.find(candidate.index);
			if (touch_it == cache_touch.end())
				continue;
			if (touch_it->second != candidate.touch)
				continue;
			victim = candidate.index;
			break;
		}
		if (victim == block_count)
			break;

		cache_blocks.erase(victim);
		cache_touch.erase(victim);
		current_cache_bytes -= block_bytes;
		--current_cache_entries;
		++metrics_evictions;
	}
}

AudioSpectrumAnalysisCache::BlockHandle AudioSpectrumAnalysisCache::Get(size_t block_index) {
	MutableBlock built;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_count == 0)
			return {};
		block_index = std::min(block_index, block_count - 1);
		auto block_it = cache_blocks.find(block_index);
		if (block_it != cache_blocks.end() && block_it->second) {
			TouchLocked(block_index);
			++metrics_cache_hits;
			return block_it->second;
		}
	}

	{
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
			auto block_it = cache_blocks.find(block_index);
			if (block_it != cache_blocks.end() && block_it->second) {
				TouchLocked(block_index);
				++metrics_cache_hits;
				return block_it->second;
			}
		}
		built = BuildBlock(block_index);

		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index >= block_count)
			return {};
		++metrics_cache_misses;
		++metrics_visible_builds;
		if (ShouldDeferSilentBlockLocked(block_index, built.get()))
			return built;
		auto block_it = cache_blocks.find(block_index);
		if (block_it == cache_blocks.end() || !block_it->second) {
			BlockHandle published = std::move(built);
			block_it = cache_blocks.emplace(block_index, std::move(published)).first;
			current_cache_bytes += BlockBytes();
			++current_cache_entries;
		}
		TouchLocked(block_index);
		TrimLocked();
		return block_it->second;
	}
	return {};
}

AudioSpectrumAnalysisCache::BlockHandle AudioSpectrumAnalysisCache::GetIfReady(size_t block_index) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_count == 0 || block_index >= block_count)
		return {};
	auto block_it = cache_blocks.find(block_index);
	if (block_it == cache_blocks.end() || !block_it->second)
		return {};
	return block_it->second;
}

bool AudioSpectrumAnalysisCache::AreBlocksReady(size_t first_block, size_t last_block) {
	if (last_block < first_block)
		return true;

	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_count == 0 || first_block >= block_count)
		return false;

	last_block = std::min(last_block, block_count - 1);
	for (size_t block_index = first_block; block_index <= last_block; ++block_index) {
		auto block_it = cache_blocks.find(block_index);
		if (block_it == cache_blocks.end() || !block_it->second)
			return false;
	}
	return true;
}

void AudioSpectrumAnalysisCache::Prefetch(size_t first_block, size_t last_block) {
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
	auto const generation = scheduler->CurrentGeneration() + 1;
	active_prefetch_generation.store(generation, std::memory_order_release);
	has_active_prefetch_range = true;
	active_prefetch_first = first_block;
	active_prefetch_last = last_block;
	scheduler->Request(first_block, last_block);
}

void AudioSpectrumAnalysisCache::SetPrefetchEnabled(bool enabled) {
	prefetch_enabled.store(enabled, std::memory_order_relaxed);
	if (!enabled)
		StopScheduler();
}

void AudioSpectrumAnalysisCache::SetPrefetchBuildMaxBlocks(size_t max_blocks) {
	prefetch_build_max_blocks.store(
		std::max<size_t>(1, std::min(kSpectrumPrefetchHardMaxBlocks, max_blocks)),
		std::memory_order_relaxed);
}

void AudioSpectrumAnalysisCache::SetReadyCallback(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(ready_callback_mutex);
	ready_callback = std::move(callback);
}

bool AudioSpectrumAnalysisCache::IsCurrentPrefetchGeneration(uint64_t generation) const {
	return generation != 0
		&& generation == active_prefetch_generation.load(std::memory_order_acquire);
}

void AudioSpectrumAnalysisCache::NotifyReady() const {
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
		LOG_W("audio/spectrum/prefetch") << "Spectrum prefetch ready callback failed";
	}
}

void AudioSpectrumAnalysisCache::ProcessPrefetch(
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
			auto const found = cache_blocks.find(block_index);
			if (found != cache_blocks.end() && found->second)
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
			auto const found = cache_blocks.find(block_index);
			if (found != cache_blocks.end() && found->second)
				return true;
		}

		MutableBlock built;
		try {
			built = BuildBlock(block_index);
		}
		catch (...) {
			rolling_window_valid = false;
			record_busy();
			return false;
		}
		if (!IsCurrentPrefetchGeneration(generation)) {
			record_stale();
			return false;
		}

		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index >= block_count)
			return false;
		auto found = cache_blocks.find(block_index);
		if (found == cache_blocks.end() || !found->second) {
			++metrics_prefetch_builds;
			if (ShouldDeferSilentBlockLocked(block_index, built.get()))
				return true;
			BlockHandle published = std::move(built);
			found = cache_blocks.emplace(block_index, std::move(published)).first;
			current_cache_bytes += BlockBytes();
			++current_cache_entries;
			built_any = true;
		}
		TouchLocked(block_index);
		TrimLocked();
		return true;
	};

	auto hint_and_build = [&](size_t range_first, size_t range_last) {
		if (range_last < range_first || !IsCurrentPrefetchGeneration(generation))
			return false;
		try {
			auto const half_window = static_cast<int64_t>(BinCount());
			auto const start_sample = std::max<int64_t>(
				0,
				(static_cast<int64_t>(range_first) << derivation_dist) - half_window);
			auto const end_sample = std::min<int64_t>(
				source->GetNumSamples(),
				(static_cast<int64_t>(range_last) << derivation_dist) + half_window);
			if (end_sample > start_sample)
				source->HintFloatAudio(start_sample, end_sample - start_sample);
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

	if (last_block >= first_block) {
		auto const max_blocks = prefetch_build_max_blocks.load(std::memory_order_relaxed);
		auto const span_minus_one = last_block - first_block;
		if (span_minus_one < max_blocks) {
			hint_and_build(first_block, last_block);
		}
		else {
			auto const lower_count = (max_blocks + 1) / 2;
			auto const upper_count = max_blocks - lower_count;
			bool const lower_complete = hint_and_build(
				first_block,
				first_block + lower_count - 1);
			if (lower_complete && upper_count > 0)
				hint_and_build(last_block - upper_count + 1, last_block);
		}
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

AudioSpectrumAnalysisCacheMetrics AudioSpectrumAnalysisCache::GetMetricsSnapshot() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	AudioSpectrumAnalysisCacheMetrics m;
	m.generation = metrics_generation;
	m.cache_hits = metrics_cache_hits;
	m.cache_misses = metrics_cache_misses;
	m.visible_builds = metrics_visible_builds;
	m.visible_lock_contention = metrics_visible_lock_contention;
	m.prefetch_requests = metrics_prefetch_requests;
	m.prefetch_builds = metrics_prefetch_builds;
	m.prefetch_busy_skips = metrics_prefetch_busy_skips;
	m.prefetch_enabled = prefetch_enabled.load(std::memory_order_relaxed);
	m.stale_drops = metrics_stale_drops;
	m.evictions = metrics_evictions;
	m.cache_entries = current_cache_entries;
	m.cache_bytes = current_cache_bytes;
	m.cache_budget_bytes = max_cache_bytes;
	return m;
}
