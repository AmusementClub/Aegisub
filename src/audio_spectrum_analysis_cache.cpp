#include "audio_spectrum_analysis_cache.h"

#include "fft.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float spectrum_eps = 1e-12f;
}

AudioSpectrumAnalysisCache::AudioSpectrumAnalysisCache() {
	scheduler = std::make_unique<AudioLatestRangeScheduler>([this](size_t first, size_t last, uint64_t generation) {
		ProcessPrefetch(first, last, generation);
	});
}

AudioSpectrumAnalysisCache::~AudioSpectrumAnalysisCache() = default;

void AudioSpectrumAnalysisCache::RecreateCache() {
	std::lock_guard<std::mutex> lock(cache_mutex);
	cache_blocks.clear();
	cache_touch.clear();
	current_cache_bytes = 0;
	touch_counter = 0;
	rolling_window_valid = false;
	{
		std::lock_guard<std::mutex> ready_lock(ready_mutex);
		ready_blocks.clear();
		has_ready_blocks = false;
	}
	if (!source || source->GetSampleRate() <= 0 || source->GetNumSamples() <= 0 || derivation_size == 0) {
		block_count = 0;
		metrics_generation.fetch_add(1, std::memory_order_relaxed);
		scheduler->Invalidate();
		return;
	}

	block_count = static_cast<size_t>((source->GetNumSamples() + ((size_t)1 << derivation_dist) - 1) >> derivation_dist);
	if (block_count == 0)
		block_count = 1;
	cache_blocks.resize(block_count);
	cache_touch.resize(block_count);
	metrics_generation.fetch_add(1, std::memory_order_relaxed);
	scheduler->Invalidate();
}

std::unique_ptr<float[]> AudioSpectrumAnalysisCache::BuildBlock(size_t block_index) {
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

	std::vector<float> fft_scratch(3 * sample_count);
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
	return block;
}

void AudioSpectrumAnalysisCache::TouchLocked(size_t block_index) {
	cache_touch[block_index] = ++touch_counter;
}

void AudioSpectrumAnalysisCache::TrimLocked() {
	const size_t block_bytes = (sizeof(float) << derivation_size);
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
	auto built = BuildBlock(block_index);
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
	if (!IsReady() || last_block < first_block)
		return;
	metrics_prefetch_requests.fetch_add(last_block - first_block + 1, std::memory_order_relaxed);
	scheduler->Request(first_block, last_block);
}

void AudioSpectrumAnalysisCache::ProcessPrefetch(size_t first_block, size_t last_block, uint64_t generation) {
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

AudioSpectrumAnalysisCacheMetrics AudioSpectrumAnalysisCache::GetMetricsSnapshot() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	AudioSpectrumAnalysisCacheMetrics m;
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
