#include "audio_spectrum_analysis_cache.h"

#ifdef WITH_FFTW3
#include "audio_spectrum_fftw3.h"
#endif

#include "fft.h"

#include <algorithm>
#include <cmath>

namespace {
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
	DestroyFftResources();
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
	touch_heap = {};
	current_cache_bytes = 0;
	current_cache_entries = 0;
	touch_counter = 0;
	rolling_window_valid = false;
}

void AudioSpectrumAnalysisCache::RecreateCache() {
	std::lock_guard<std::mutex> lock(cache_mutex);
	ClearLocked();
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

#ifdef WITH_PFFFT
	if (!has_fftw_plan) {
		const size_t pffft_bytes = WindowSampleCount() * sizeof(float);
		pffft_setup = pffft_new_setup(static_cast<int>(WindowSampleCount()), PFFFT_REAL);
		if (pffft_setup) {
			pffft_input = static_cast<float *>(pffft_aligned_malloc(pffft_bytes));
			pffft_output = static_cast<float *>(pffft_aligned_malloc(pffft_bytes));
			pffft_work = static_cast<float *>(pffft_aligned_malloc(pffft_bytes));
			if (!pffft_input || !pffft_output || !pffft_work)
				DestroyFftResources();
		}
		if (!pffft_setup)
			fft_scratch.resize(WindowSampleCount() * 3);
	}
#else
	if (!has_fftw_plan)
		fft_scratch.resize(WindowSampleCount() * 3);
#endif

	++metrics_generation;
}

void AudioSpectrumAnalysisCache::SetSource(AudioDisplaySource *new_source) {
	std::lock_guard<std::mutex> build_lock(build_mutex);
	if (source == new_source)
		return;
	source = new_source;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::SetMixPolicy(AudioMixPolicy new_policy) {
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
	std::lock_guard<std::mutex> build_lock(build_mutex);
	new_derivation_dist = std::min(new_derivation_dist, new_derivation_size);
	if (derivation_size == new_derivation_size && derivation_dist == new_derivation_dist)
		return;
	derivation_size = new_derivation_size;
	derivation_dist = new_derivation_dist;
	RecreateCache();
}

void AudioSpectrumAnalysisCache::Age(size_t max_size) {
	std::lock_guard<std::mutex> build_lock(build_mutex);
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (max_size > 0) {
		max_cache_bytes = std::max(BlockBytes(), max_size);
		TrimLocked();
	}
	else {
		ClearLocked();
	}
}

bool AudioSpectrumAnalysisCache::IsReady() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return source && block_count > 0 && derivation_size > 0;
}

AudioSpectrumAnalysisCache::CacheBlock AudioSpectrumAnalysisCache::BuildBlock(size_t block_index) {
	const int channels = std::max(1, source->GetChannels());
	const size_t sample_count = WindowSampleCount();
	const size_t hop_samples = HopSampleCount();
	const size_t bin_count = BinCount();
	auto block = std::make_unique<float[]>(bin_count);

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

const float* AudioSpectrumAnalysisCache::Get(size_t block_index) {
	CacheBlock built;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_count == 0)
			return nullptr;
		block_index = std::min(block_index, block_count - 1);
		auto block_it = cache_blocks.find(block_index);
		if (block_it != cache_blocks.end() && block_it->second) {
			TouchLocked(block_index);
			++metrics_cache_hits;
			return block_it->second.get();
		}
	}

	{
		std::lock_guard<std::mutex> build_lock(build_mutex);
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			if (block_index >= block_count)
				return nullptr;
			auto block_it = cache_blocks.find(block_index);
			if (block_it != cache_blocks.end() && block_it->second) {
				TouchLocked(block_index);
				++metrics_cache_hits;
				return block_it->second.get();
			}
		}
		built = BuildBlock(block_index);

		std::lock_guard<std::mutex> lock(cache_mutex);
		if (block_index >= block_count)
			return nullptr;
		auto block_it = cache_blocks.find(block_index);
		if (block_it == cache_blocks.end() || !block_it->second) {
			block_it = cache_blocks.emplace(block_index, std::move(built)).first;
			current_cache_bytes += BlockBytes();
			++current_cache_entries;
			++metrics_cache_misses;
			++metrics_visible_builds;
		}
		TouchLocked(block_index);
		TrimLocked();
		return block_it->second.get();
	}
	return nullptr;
}

const float* AudioSpectrumAnalysisCache::GetIfReady(size_t block_index) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (block_count == 0 || block_index >= block_count)
		return nullptr;
	auto block_it = cache_blocks.find(block_index);
	if (block_it == cache_blocks.end() || !block_it->second)
		return nullptr;
	return block_it->second.get();
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
	if (!prefetch_enabled || !source || last_block < first_block)
		return;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		++metrics_prefetch_requests;
	}

	const auto half_window = static_cast<int64_t>(BinCount());
	const auto start_sample = std::max<int64_t>(0, (static_cast<int64_t>(first_block) << derivation_dist) - half_window);
	const auto end_sample = std::min<int64_t>(
		source->GetNumSamples(),
		(static_cast<int64_t>(last_block) << derivation_dist) + half_window);
	if (end_sample <= start_sample)
		return;
	source->HintFloatAudio(start_sample, end_sample - start_sample);
}

void AudioSpectrumAnalysisCache::SetPrefetchEnabled(bool enabled) {
	prefetch_enabled = enabled;
}

AudioSpectrumAnalysisCacheMetrics AudioSpectrumAnalysisCache::GetMetricsSnapshot() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	AudioSpectrumAnalysisCacheMetrics m;
	m.generation = metrics_generation;
	m.cache_hits = metrics_cache_hits;
	m.cache_misses = metrics_cache_misses;
	m.visible_builds = metrics_visible_builds;
	m.visible_lock_contention = 0;
	m.prefetch_requests = metrics_prefetch_requests;
	m.prefetch_builds = 0;
	m.prefetch_busy_skips = 0;
	m.prefetch_enabled = prefetch_enabled;
	m.stale_drops = 0;
	m.evictions = metrics_evictions;
	m.cache_entries = current_cache_entries;
	m.cache_bytes = current_cache_bytes;
	return m;
}
