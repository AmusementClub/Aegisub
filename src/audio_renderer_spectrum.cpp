// Copyright (c) 2005-2006, Rodrigo Braz Monteiro
// Copyright (c) 2006-2010, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file audio_renderer_spectrum.cpp
/// @brief Caching frequency-power spectrum renderer for audio display
/// @ingroup audio_ui

#include "audio_renderer_spectrum.h"

#include "audio_colorscheme.h"
#include "audio_display_analysis.h"
#include "audio_display_source.h"
#include "audio_spectrum_analysis_cache.h"
#include "utils.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include <wx/dcmemory.h>
#include <wx/image.h>

namespace {
void RenderSpectrumColumnsToBitmap(
	wxBitmap &bmp,
	const std::vector<const float *> &columns,
	const int *band_a,
	const int *band_b,
	const float *band_frac,
	bool interpolated,
	float amplitude_scale,
	AudioColorScheme const& palette)
{
	assert(bmp.IsOk());

	wxImage img(bmp.GetSize());
	unsigned char *imgdata = img.GetData();
	ptrdiff_t stride = img.GetWidth() * 3;
	int imgheight = img.GetHeight();

	for (int x = 0; x < bmp.GetWidth(); ++x) {
		const float *power = columns[static_cast<size_t>(x)];
		unsigned char *px = imgdata + (imgheight - 1) * stride + x * 3;

		if (interpolated) {
			for (int y = 0; y < imgheight; ++y) {
				float val = 0.f;
				if (power) {
					float const frac = band_frac[y];
					val = (1.f - frac) * power[band_a[y]] + frac * power[band_b[y]];
				}
				palette.map(val * amplitude_scale, px);
				px -= stride;
			}
		}
		else {
			for (int y = 0; y < imgheight; ++y) {
				float val = 0.f;
				if (power)
					val = *std::max_element(&power[band_a[y]], &power[band_b[y] + 1]);
				palette.map(val * amplitude_scale, px);
				px -= stride;
			}
		}
	}

	wxBitmap tmpbmp(img);
	wxMemoryDC targetdc(bmp);
	targetdc.DrawBitmap(tmpbmp, 0, 0);
}
}

AudioSpectrumRenderer::AudioSpectrumRenderer(std::string const& color_scheme_name) {
	colors.reserve(AudioStyle_MAX);
	for (int i = 0; i < AudioStyle_MAX; ++i)
		colors.emplace_back(12, color_scheme_name, i);
	analysis_cache = std::make_unique<AudioSpectrumAnalysisCache>();
}

AudioSpectrumRenderer::~AudioSpectrumRenderer() {
}

bool AudioSpectrumRenderer::UsesPerChannelMonoAggregation() const {
	return input_format == AudioSpectrumInputFormat::Float32
		&& (mono_mix_mode == AudioSpectrumMonoMixMode::PerBinMaxPower
		|| mono_mix_mode == AudioSpectrumMonoMixMode::PerBinAveragePower);
}

void AudioSpectrumRenderer::SetFrequencyReferencePosition(float position) {
	frequency_reference_position = mid(0.001f, position, 0.999f);
}

void AudioSpectrumRenderer::EnsureRenderScaleCache(int imgheight) {
	const int sample_rate = display_source ? display_source->GetSampleRate() : 0;
	bool interpolated = imgheight > (1 << derivation_size);
	if (render_scale_cache_height == imgheight
		&& render_scale_cache_derivation_size == derivation_size
		&& render_scale_cache_interpolated == interpolated
		&& render_scale_cache_sample_rate == sample_rate
		&& render_scale_cache_mode == static_cast<int>(computation_mode)
		&& render_scale_cache_reference_position == frequency_reference_position)
		return;

	render_scale_cache_height = imgheight;
	render_scale_cache_derivation_size = derivation_size;
	render_scale_cache_interpolated = interpolated;
	render_scale_cache_sample_rate = sample_rate;
	render_scale_cache_mode = static_cast<int>(computation_mode);
	render_scale_cache_reference_position = frequency_reference_position;

	render_band_a.resize(imgheight);
	render_band_b.resize(imgheight);
	render_band_frac.resize(interpolated ? imgheight : 0);

	if (computation_mode == AudioSpectrumComputationMode::LegacyLinear || !display_source || sample_rate <= 0) {
		const int maxband = 1 << derivation_size;
		if (interpolated) {
			for (int y = 0; y < imgheight; ++y) {
				double ideal = static_cast<double>(y + 1.) / imgheight * maxband;
				int lower = std::max(0, std::min(maxband - 1, static_cast<int>(std::floor(ideal))));
				int upper = std::max(0, std::min(maxband - 1, static_cast<int>(std::ceil(ideal))));
				render_band_a[y] = lower;
				render_band_b[y] = upper;
				render_band_frac[y] = static_cast<float>(ideal - std::floor(ideal));
			}
		}
		else {
			for (int y = 0; y < imgheight; ++y) {
				int sample1 = std::max(0, maxband * y / imgheight);
				int sample2 = std::min(maxband - 1, maxband * (y + 1) / imgheight);
				render_band_a[y] = sample1;
				render_band_b[y] = sample2;
			}
		}
		return;
	}

	const int bin_count = 1 << derivation_size;
	const int minband = 1;
	int maxband = std::min(bin_count, static_cast<int>(std::floor(bin_count * 20000.0f / (sample_rate * 0.5f))));
	if (maxband <= minband + 1)
		maxband = std::min(bin_count, minband + 2);

	const float scale_log = std::log(static_cast<float>(maxband) / minband);
	const float b_fref = mid(1.0f, bin_count * 1000.0f / (sample_rate * 0.5f), static_cast<float>(maxband - 1));
	const float b_lin_fref = minband + (maxband - minband) * frequency_reference_position;
	const float b_log_fref = minband * std::exp(frequency_reference_position * scale_log);
	float log_ratio = (b_fref - b_lin_fref) / (b_log_fref - b_lin_fref);
	log_ratio = mid(0.0f, log_ratio, 1.0f);

	auto mapped_bin = [&](float pos_rel) {
		float b_lin = minband + pos_rel * (maxband - minband);
		float b_log = minband * std::exp(pos_rel * scale_log);
		float bin = b_lin + log_ratio * (b_log - b_lin);
		return mid(static_cast<float>(minband), bin, static_cast<float>(maxband - 1));
	};

	if (interpolated) {
		for (int y = 0; y < imgheight; ++y) {
			float bin = mapped_bin(static_cast<float>(y + 1) / imgheight);
			int lower = std::max(0, std::min(bin_count - 1, static_cast<int>(std::floor(bin))));
			int upper = std::max(0, std::min(bin_count - 1, static_cast<int>(std::ceil(bin))));
			render_band_a[y] = lower;
			render_band_b[y] = upper;
			render_band_frac[y] = bin - std::floor(bin);
		}
	}
	else {
		for (int y = 0; y < imgheight; ++y) {
			float bin_prev = y == 0 ? static_cast<float>(minband) : mapped_bin(static_cast<float>(y) / imgheight);
			float bin_cur = mapped_bin(static_cast<float>(y + 1) / imgheight);
			float bin_next = y + 2 <= imgheight ? mapped_bin(static_cast<float>(y + 2) / imgheight) : static_cast<float>(maxband);

			int sample1 = static_cast<int>(std::floor((bin_prev + bin_cur) * 0.5f));
			int sample2 = static_cast<int>(std::floor((bin_cur + bin_next) * 0.5f));
			sample1 = std::max(0, std::min(bin_count - 2, sample1));
			sample2 = std::max(sample1 + 1, std::min(bin_count - 1, sample2));
			render_band_a[y] = sample1;
			render_band_b[y] = sample2;
		}
	}
}

void AudioSpectrumRenderer::EnsurePerChannelCaches() {
	per_channel_caches.clear();
	per_channel_sources.clear();

	if (!UsesPerChannelMonoAggregation() || !display_source)
		return;

	const int channels = std::max(1, display_source->GetChannels());
	per_channel_sources.reserve(channels);
	per_channel_caches.reserve(channels);
	for (int ch = 0; ch < channels; ++ch) {
		per_channel_sources.push_back(CreateSingleChannelAudioDisplaySource(display_source.get(), ch));
		auto cache = std::make_unique<AudioSpectrumAnalysisCache>();
		cache->SetSource(per_channel_sources.back().get());
		cache->SetResolution(derivation_size, derivation_dist);
		per_channel_caches.push_back(std::move(cache));
	}
}

void AudioSpectrumRenderer::RecreateDisplaySource() {
	if (analysis_cache)
		analysis_cache->SetSource(nullptr);
	per_channel_caches.clear();
	per_channel_sources.clear();

	if (input_format == AudioSpectrumInputFormat::Float32)
		display_source = CreateAudioDisplaySource(provider);
	else
		display_source = CreateInt16MonoAudioDisplaySource(provider);
}

void AudioSpectrumRenderer::RecreateCache() {
	if (UsesPerChannelMonoAggregation()) {
		if (analysis_cache)
			analysis_cache->Age(0);
	} else if (analysis_cache) {
		analysis_cache->SetSource(display_source.get());
		analysis_cache->SetResolution(derivation_size, derivation_dist);
	}
	EnsurePerChannelCaches();
}

void AudioSpectrumRenderer::OnSetProvider() {
	RecreateDisplaySource();
	RecreateCache();
	render_scale_cache_height = 0;
}

void AudioSpectrumRenderer::SetResolution(size_t new_derivation_size, size_t new_derivation_dist) {
	new_derivation_dist = std::min(new_derivation_dist, new_derivation_size);
	if (derivation_size == new_derivation_size && derivation_dist == new_derivation_dist)
		return;
	derivation_size = new_derivation_size;
	derivation_dist = new_derivation_dist;
	RecreateCache();
	render_scale_cache_height = 0;
}

void AudioSpectrumRenderer::SetComputationMode(AudioSpectrumComputationMode mode) {
	if (computation_mode == mode)
		return;
	computation_mode = mode;
	render_scale_cache_height = 0;
}

void AudioSpectrumRenderer::SetFrequencyCurvePreset(int preset) {
	preset = mid(0, preset, 4);
	if (frequency_curve_preset == preset)
		return;
	frequency_curve_preset = preset;
	const float fref_pos[] = { 0.001f, 0.125f, 0.333f, 0.425f, 0.999f };
	SetFrequencyReferencePosition(fref_pos[preset]);
	render_scale_cache_height = 0;
}

void AudioSpectrumRenderer::SetMonoMixMode(AudioSpectrumMonoMixMode mode) {
	if (mono_mix_mode == mode)
		return;
	mono_mix_mode = mode;
	EnsurePerChannelCaches();
	AgeCache(0);
}

void AudioSpectrumRenderer::SetInputFormat(AudioSpectrumInputFormat path) {
	if (input_format == path)
		return;
	input_format = path;
	RecreateDisplaySource();
	RecreateCache();
	render_scale_cache_height = 0;
}

void AudioSpectrumRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style) {
	if (!display_source)
		return;

	assert(bmp.IsOk());
	assert(start >= 0);

	int const width = bmp.GetWidth();
	int const end = start + width;
	const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
	const AudioColorScheme *pal = &colors[style];
	EnsureRenderScaleCache(bmp.GetHeight());
	const bool interpolated = bmp.GetHeight() > (1 << derivation_size);

	if (UsesPerChannelMonoAggregation()) {
		if (per_channel_caches.empty())
			EnsurePerChannelCaches();
		if (per_channel_caches.empty())
			return;

		const size_t bin_count = static_cast<size_t>(1) << derivation_size;
		combined_power_scratch.resize(static_cast<size_t>(width) * bin_count);
		combined_power_columns.resize(width);
		channel_power_inputs.resize(per_channel_caches.size());
		channel_power_blocks.resize(per_channel_caches.size());

		size_t last_block_index = static_cast<size_t>(-1);
		const float *last_power = nullptr;
		for (int ax = start; ax < end; ++ax) {
			size_t block_index = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
			size_t column = static_cast<size_t>(ax - start);
			if (block_index == last_block_index) {
				combined_power_columns[column] = last_power;
				continue;
			}

			for (size_t ch = 0; ch < per_channel_caches.size(); ++ch) {
				channel_power_blocks[ch] = per_channel_caches[ch]
					? per_channel_caches[ch]->Get(block_index)
					: AudioSpectrumAnalysisCache::BlockHandle {};
				channel_power_inputs[ch] = channel_power_blocks[ch].get();
			}

			float *dst = combined_power_scratch.data() + column * bin_count;
			if (mono_mix_mode == AudioSpectrumMonoMixMode::PerBinMaxPower)
				MergeSpectrumPowerBinsMax(channel_power_inputs, bin_count, dst);
			else
				MergeSpectrumPowerBinsAverage(channel_power_inputs, bin_count, dst);

			combined_power_columns[column] = dst;
			last_block_index = block_index;
			last_power = dst;
		}

		RenderSpectrumColumnsToBitmap(
			bmp,
			combined_power_columns,
			render_band_a.data(),
			render_band_b.data(),
			interpolated ? render_band_frac.data() : nullptr,
			interpolated,
			amplitude_scale,
			*pal);
		channel_power_blocks.clear();
		return;
	}

	if (!analysis_cache || !analysis_cache->IsReady())
		return;

	power_columns.resize(width);
	power_blocks.resize(width);
	size_t last_block_index = static_cast<size_t>(-1);
	AudioSpectrumAnalysisCache::BlockHandle last_power;
	for (int ax = start; ax < end; ++ax) {
		size_t block_index = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
		if (block_index != last_block_index) {
			last_power = analysis_cache->Get(block_index);
			last_block_index = block_index;
		}
		auto const column = static_cast<size_t>(ax - start);
		power_blocks[column] = last_power;
		power_columns[column] = last_power.get();
	}

	RenderSpectrumColumnsToBitmap(
		bmp,
		power_columns,
		render_band_a.data(),
		render_band_b.data(),
		interpolated ? render_band_frac.data() : nullptr,
		interpolated,
		amplitude_scale,
		*pal);
	power_blocks.clear();
}

void AudioSpectrumRenderer::RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style) {
	wxColour col = colors[style].get(0.0f);
	dc.SetBrush(wxBrush(col));
	dc.SetPen(wxPen(col));
	dc.DrawRectangle(rect);
}

void AudioSpectrumRenderer::AgeCache(size_t max_size) {
	if (UsesPerChannelMonoAggregation()) {
		if (analysis_cache)
			analysis_cache->Age(0);
		const size_t cache_count = per_channel_caches.size();
		const size_t per_cache_max = cache_count > 0 ? max_size / cache_count : max_size;
		for (auto &cache : per_channel_caches) {
			if (cache)
				cache->Age(per_cache_max);
		}
	}
	else {
		if (analysis_cache)
			analysis_cache->Age(max_size);
		for (auto &cache : per_channel_caches) {
			if (cache)
				cache->Age(0);
		}
	}
}

void AudioSpectrumRenderer::Prefetch(int start, int length) {
	if (!display_source || pixel_ms <= 0.0 || start < 0 || length <= 0)
		return;

	auto const samples_per_pixel = static_cast<long double>(pixel_ms)
		* display_source->GetSampleRate() / 1000.0L;
	auto const last_column = static_cast<int64_t>(start) + length - 1;
	auto const first_sample = static_cast<long double>(start) * samples_per_pixel;
	auto const last_sample = static_cast<long double>(last_column) * samples_per_pixel;
	if (!std::isfinite(samples_per_pixel)
		|| samples_per_pixel <= 0.0L
		|| first_sample < 0.0L
		|| last_sample < first_sample
		|| last_sample > static_cast<long double>(std::numeric_limits<size_t>::max())) {
		return;
	}

	auto first_block = static_cast<size_t>(first_sample) >> derivation_dist;
	auto last_block = static_cast<size_t>(last_sample) >> derivation_dist;
	constexpr size_t margin_blocks = 32;
	first_block = first_block > margin_blocks ? first_block - margin_blocks : 0;
	last_block = last_block <= std::numeric_limits<size_t>::max() - margin_blocks
		? last_block + margin_blocks
		: std::numeric_limits<size_t>::max();

	if (UsesPerChannelMonoAggregation()) {
		for (auto &cache : per_channel_caches) {
			if (cache)
				cache->Prefetch(first_block, last_block);
		}
	}
	else if (analysis_cache) {
		analysis_cache->Prefetch(first_block, last_block);
	}
}

bool AudioSpectrumRenderer::GetCacheMetrics(AudioRendererCacheMetrics &metrics) const {
	metrics.content_kind = AudioRendererContentKind::Spectrum;
	metrics.prefetch_enabled = true;
	size_t cache_count = 0;
	auto append = [&](AudioSpectrumAnalysisCache const& cache) {
		auto const source = cache.GetMetricsSnapshot();
		++cache_count;
		metrics.generation = std::max(metrics.generation, source.generation);
		metrics.source_cache_hits += source.cache_hits;
		metrics.source_cache_misses += source.cache_misses;
		metrics.visible_builds += source.visible_builds;
		metrics.visible_lock_contention += source.visible_lock_contention;
		metrics.prefetch_requests += source.prefetch_requests;
		metrics.prefetch_builds += source.prefetch_builds;
		metrics.prefetch_busy_skips += source.prefetch_busy_skips;
		metrics.stale_drops += source.stale_drops;
		metrics.evictions += source.evictions;
		metrics.cache_entries += source.cache_entries;
		metrics.cache_bytes += source.cache_bytes;
		metrics.cache_budget_bytes += source.cache_budget_bytes;
		metrics.prefetch_enabled = metrics.prefetch_enabled && source.prefetch_enabled;
	};

	if (UsesPerChannelMonoAggregation()) {
		for (auto const& cache : per_channel_caches) {
			if (cache)
				append(*cache);
		}
	}
	else if (analysis_cache) {
		append(*analysis_cache);
	}
	return cache_count != 0;
}
