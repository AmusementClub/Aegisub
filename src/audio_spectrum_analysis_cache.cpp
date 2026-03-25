#include "audio_spectrum_analysis_cache.h"

#include "fft.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float spectrum_eps = 1e-12f;
}

AudioSpectrumAnalysisCache::AudioSpectrumAnalysisCache() {
}

AudioSpectrumAnalysisCache::~AudioSpectrumAnalysisCache() {
#ifdef WITH_FFTW3
	if (dft_plan) {
		fftw_destroy_plan(dft_plan);
		fftw_free(dft_input);
		fftw_free(dft_output);
	}
#endif
}

void AudioSpectrumAnalysisCache::RecreateCache() {
	std::lock_guard<std::mutex> lock(cache_mutex);
	cache_blocks.clear();
	cache_touch.clear();
	touch_heap = {};
	current_cache_bytes = 0;
	touch_counter = 0;
	rolling_window_valid = false;
#ifdef WITH_FFTW3
	if (dft_plan) {
		fftw_destroy_plan(dft_plan);
		fftw_free(dft_input);
		fftw_free(dft_output);
		dft_plan = nullptr;
		dft_input = nullptr;
		dft_output = nullptr;
	}
#endif
	{
		std::lock_guard<std::mutex> ready_lock(ready_mutex);
		ready_blocks.clear();
		has_ready_blocks = false;
	}
	if (!source || source->GetSampleRate() <= 0 || source->GetNumSamples() <= 0 || derivation_size == 0) {
		block_count = 0;
		metrics_generation.fetch_add(1, std::memory_order_relaxed);
		if (scheduler)
			scheduler->Invalidate();
		return;
	}

	block_count = static_cast<size_t>((source->GetNumSamples() + ((size_t)1 << derivation_dist) - 1) >> derivation_dist);
	if (block_count == 0)
		block_count = 1;
	cache_blocks.resize(block_count);
	cache_touch.resize(block_count);

#ifdef WITH_FFTW3
	dft_input = fftw_alloc_real(2 << derivation_size);
	dft_output = fftw_alloc_complex(2 << derivation_size);
	dft_plan = fftw_plan_dft_r2c_1d(
		2 << derivation_size,
		dft_input,
		dft_output,
		FFTW_MEASURE);
#else
	fft_scratch.resize(6 << derivation_size);
#endif

	metrics_generation.fetch_add(1, std::memory_order_relaxed);
	if (scheduler)
		scheduler->Invalidate();
}

std::unique_ptr<float[]> AudioSpectrumAnalysisCache::BuildBlockUnlocked(size_t block_index) {
	const int channels = std::max(1, source->GetChannels());
	const size_t sample_count = static_cast<size_t>(2) << derivation_size;
	const size_t hop_samples = static_cast<size_t>(1) << derivation_dist;
	auto block = std::make_unique<float[]>(static_cast<size_t>(1) << derivation_size);
	mono_scratch.resize(sample_count);

	int64_t first_sample = (((int64_t)block_index) << derivation_dist) - ((int64_t)1 << derivation_size);
	bool reused_window = rolling_window_valid
		&& block_index == rolling_window_block_index + 1
		&& hop_samples <= sample_count;

	if (reused_window) {
		const size_t overlap_samples = sample_count - hop_samples;
		std::move(mono_scratch.begin() + hop_samples, mono_scratch.end(), mono_scratch.begin());
		audio_scratch.resize(hop_samples * channels);
		source->GetFloatAudio(audio_scratch.data(), first_sample + overlap_samples, hop_samples);
		float *tail = mono_scratch.data() + overlap_samples;
		if (channels == 1)
			std::copy(audio_scratch.begin(), audio_scratch.begin() + hop_samples, tail);
		else
			MixAudioToMono(mix_policy, audio_scratch.data(), static_cast<int>(hop_samples), channels, tail);
	}
	else {
		audio_scratch.resize(sample_count * channels);
		source->GetFloatAudio(audio_scratch.data(), first_sample, sample_count);
		if (channels == 1)
			std::copy(audio_scratch.begin(), audio_scratch.begin() + sample_count, mono_scratch.begin());
		else
			MixAudioToMono(mix_policy, audio_scratch.data(), static_cast<int>(sample_count), channels, mono_scratch.data());
	}

	rolling_window_valid = true;
	rolling_window_block_index = block_index;

#ifdef WITH_FFTW3
	for (size_t i = 0; i < sample_count; ++i)
		dft_input[i] = mono_scratch[i];

	fftw_execute(dft_plan);

	double scale_factor = 9 / std::sqrt(2 << (derivation_size + 1));
	fftw_complex *o = dft_output;
	for (size_t i = 0; i < (static_cast<size_t>(1) << derivation_size); ++i, ++o)
		block[i] = std::log10(std::sqrt(static_cast<float>(o[0][0] * o[0][0] + o[0][1] * o[0][1])) * static_cast<float>(scale_factor) + 1.f);
#else
	float *fft_input = fft_scratch.data();
	float *fft_real = fft_scratch.data() + sample_count;
	float *fft_imag = fft_scratch.data() + sample_count * 2;
	std::copy(mono_scratch.begin(), mono_scratch.end(), fft_input);

	FFT fft;
	fft.Transform(sample_count, fft_input, fft_real, fft_imag);
	const float scale_factor = 9 / std::sqrt(2 * static_cast<float>(sample_count));
	for (size_t i = 0; i < (static_cast<size_t>(1) << derivation_size); ++i) {
		float power = std::sqrt(fft_real[i] * fft_real[i] + fft_imag[i] * fft_imag[i]) * scale_factor;
		block[i] = std::log10(power + 1.f);
	}
#endif
	return block;
}

void AudioSpectrumAnalysisCache::TouchLocked(size_t block_index) {
	const uint64_t touch = ++touch_counter;
	cache_touch[block_index] = touch;
	touch_heap.push(TouchEntry{ touch, block_index });
}

void AudioSpectrumAnalysisCache::TrimLocked() {
	const size_t block_bytes = (sizeof(float) << derivation_size);
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

void AudioSpectrumAnalysisCache::DrainReady() {
	if (!has_ready_blocks.load(std::memory_order_acquire))
		return;
	std::vector<std::pair<size_t, std::unique_ptr<float[]>>> ready;
	{
		std::lock_guard<std::mutex> lock(ready_mutex);
		if (ready_blocks.empty())
			return;
		ready.swap(ready_blocks);
		has_ready_blocks = false;
	}

	const size_t block_bytes = (sizeof(float) << derivation_size);
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

void AudioSpectrumAnalysisCache::SetSource(AudioDisplaySource *new_source) {
	if (source == new_source)
		return;
	source = new_source;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::SetMixPolicy(AudioMixPolicy new_policy) {
	if (mix_policy == new_policy)
		return;
	mix_policy = new_policy;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::SetResolution(size_t new_derivation_size, size_t new_derivation_dist) {
	if (derivation_size == new_derivation_size && derivation_dist == new_derivation_dist)
		return;
	derivation_size = new_derivation_size;
	derivation_dist = std::min(new_derivation_dist, new_derivation_size);
	RecreateCache();
}

void AudioSpectrumAnalysisCache::Age(size_t max_size) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	const size_t block_bytes = (sizeof(float) << derivation_size);
	max_cache_bytes = std::max(block_bytes, max_size);
	TrimLocked();
}

bool AudioSpectrumAnalysisCache::IsReady() const {
	return source && block_count > 0;
}

const float* AudioSpectrumAnalysisCache::Get(size_t block_index) {
	DrainReady();
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index < cache_blocks.size() && cache_blocks[block_index]) {
			TouchLocked(block_index);
			metrics_cache_hits.fetch_add(1, std::memory_order_relaxed);
			return cache_blocks[block_index].get();
		}
	}

	metrics_cache_misses.fetch_add(1, std::memory_order_relaxed);
	std::unique_lock<std::mutex> build_lock(build_mutex, std::try_to_lock);
	if (!build_lock.owns_lock()) {
		metrics_visible_lock_contention.fetch_add(1, std::memory_order_relaxed);
		build_lock.lock();
	}
	auto built = BuildBlockUnlocked(block_index);
	metrics_visible_builds.fetch_add(1, std::memory_order_relaxed);

	const size_t block_bytes = (sizeof(float) << derivation_size);
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
	return cache_blocks[block_index].get();
}

void AudioSpectrumAnalysisCache::Prefetch(size_t first_block, size_t last_block) {
	if (!prefetch_enabled.load(std::memory_order_relaxed))
		return;
	if (!IsReady() || last_block < first_block)
		return;
	if (first_block >= block_count)
		return;
	last_block = std::min(last_block, block_count - 1);

	auto const half_window = static_cast<int64_t>(size_t(1) << derivation_size);
	auto const start_sample = (static_cast<int64_t>(first_block) << derivation_dist) - half_window;
	auto const end_sample = (static_cast<int64_t>(last_block) << derivation_dist) + half_window;
	source->HintFloatAudio(start_sample, end_sample - start_sample);

	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		bool has_missing = false;
		for (size_t i = first_block; i <= last_block; ++i) {
			if (!cache_blocks[i]) {
				has_missing = true;
				break;
			}
		}
		if (!has_missing)
			return;
	}

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

void AudioSpectrumAnalysisCache::SetPrefetchEnabled(bool enabled) {
	prefetch_enabled.store(enabled, std::memory_order_relaxed);
	if (!enabled && scheduler)
		scheduler->Invalidate();
}

void AudioSpectrumAnalysisCache::ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation) {
	if (!scheduler)
		return;
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
		std::unique_lock<std::mutex> build_lock(build_mutex, std::try_to_lock);
		if (!build_lock.owns_lock()) {
			metrics_prefetch_busy_skips.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		auto built = BuildBlockUnlocked(block_index);
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

AudioSpectrumAnalysisCacheMetrics AudioSpectrumAnalysisCache::GetMetricsSnapshot() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	AudioSpectrumAnalysisCacheMetrics m;
	m.generation = metrics_generation.load(std::memory_order_relaxed);
	m.cache_hits = metrics_cache_hits.load(std::memory_order_relaxed);
	m.cache_misses = metrics_cache_misses.load(std::memory_order_relaxed);
	m.visible_builds = metrics_visible_builds.load(std::memory_order_relaxed);
	m.visible_lock_contention = metrics_visible_lock_contention.load(std::memory_order_relaxed);
	m.prefetch_requests = metrics_prefetch_requests.load(std::memory_order_relaxed);
	m.prefetch_builds = metrics_prefetch_builds.load(std::memory_order_relaxed);
	m.prefetch_busy_skips = metrics_prefetch_busy_skips.load(std::memory_order_relaxed);
	m.prefetch_enabled = prefetch_enabled.load(std::memory_order_relaxed);
	m.stale_drops = metrics_stale_drops.load(std::memory_order_relaxed);
	m.evictions = metrics_evictions.load(std::memory_order_relaxed);
	m.cache_entries = std::count_if(cache_blocks.begin(), cache_blocks.end(), [](auto const& block) { return !!block; });
	m.cache_bytes = current_cache_bytes;
	return m;
}
