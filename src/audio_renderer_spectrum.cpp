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

#include "audio_spectrum_analysis_cache.h"
#include "audio_spectrum_bitmap_tile_renderer.h"
#include "audio_colorscheme.h"
#include "audio_display_source.h"
#include "audio_mix_policy.h"
#ifndef WITH_FFTW3
#include "fft.h"
#endif

#include <libaegisub/audio/provider.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <wx/image.h>
#include <wx/dcmemory.h>

AudioSpectrumRenderer::AudioSpectrumRenderer(std::string const& color_scheme_name)
{
	colors.reserve(AudioStyle_MAX);
	for (int i = 0; i < AudioStyle_MAX; ++i)
		colors.emplace_back(12, color_scheme_name, i);
	analysis_cache = std::make_unique<AudioSpectrumAnalysisCache>();
}

void AudioSpectrumRenderer::SetFrequencyReferencePosition(float position) {
	frequency_reference_position = mid(0.001f, position, 0.999f);
}

void AudioSpectrumRenderer::EnsureRenderScaleCache(int imgheight) {
	const int sample_rate = provider ? provider->GetSampleRate() : 0;
	bool interpolated = imgheight > 1 << derivation_size;
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

	if (computation_mode == AudioSpectrumComputationMode::LegacyLinear || !provider || sample_rate <= 0) {
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

	const int nbr_bins = 1 << derivation_size;
	const int minband = 1;
	int maxband = std::min(nbr_bins, static_cast<int>(std::floor(nbr_bins * 20000.0f / (sample_rate * 0.5f))));
	if (maxband <= minband + 1)
		maxband = std::min(nbr_bins, minband + 2);

	const float scale_log = std::log(static_cast<float>(maxband) / minband);
	const float b_fref = mid(1.0f, nbr_bins * 1000.0f / (sample_rate * 0.5f), static_cast<float>(maxband - 1));
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
			int lower = std::max(0, std::min(nbr_bins - 1, static_cast<int>(std::floor(bin))));
			int upper = std::max(0, std::min(nbr_bins - 1, static_cast<int>(std::ceil(bin))));
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
			sample1 = std::max(0, std::min(nbr_bins - 2, sample1));
			sample2 = std::max(sample1 + 1, std::min(nbr_bins - 1, sample2));
			render_band_a[y] = sample1;
			render_band_b[y] = sample2;
		}
	}
}

AudioSpectrumRenderer::~AudioSpectrumRenderer()
{
}

void AudioSpectrumRenderer::RecreateCache()
{
	if (analysis_cache) {
		analysis_cache->SetSource(display_source);
		analysis_cache->SetMixPolicy(mix_policy);
		analysis_cache->SetResolution(derivation_size, derivation_dist);
	}
}

void AudioSpectrumRenderer::OnSetProvider()
{
	RecreateCache();
}

void AudioSpectrumRenderer::SetResolution(size_t _derivation_size, size_t _derivation_dist)
{
	if (derivation_dist != _derivation_dist)
		derivation_dist = _derivation_dist;

	if (derivation_size != _derivation_size)
		derivation_size = _derivation_size;
	RecreateCache();
}

void AudioSpectrumRenderer::SetComputationMode(AudioSpectrumComputationMode mode) {
	if (computation_mode == mode)
		return;
	computation_mode = mode;
	render_scale_cache_height = 0;
	AgeCache(0);
}

void AudioSpectrumRenderer::SetFrequencyCurvePreset(int preset) {
	preset = mid(0, preset, 4);
	if (frequency_curve_preset == preset)
		return;
	frequency_curve_preset = preset;
	const float fref_pos[] = {0.001f, 0.125f, 0.333f, 0.425f, 0.999f};
	SetFrequencyReferencePosition(fref_pos[preset]);
	render_scale_cache_height = 0;
	AgeCache(0);
}

void AudioSpectrumRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style)
{
	if (!analysis_cache || !analysis_cache->IsReady())
		return;

	assert(bmp.IsOk());

	int end = start + bmp.GetWidth();

	assert(start >= 0);
	assert(end >= start);

	const AudioColorScheme *pal = &colors[style];
	const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
	EnsureRenderScaleCache(bmp.GetHeight());
	const bool interpolated = bmp.GetHeight() > 1 << derivation_size;

	std::vector<const float *> power_columns(static_cast<size_t>(bmp.GetWidth()));
	size_t last_block_index = static_cast<size_t>(-1);
	const float *last_power = nullptr;

	for (int ax = start; ax < end; ++ax) {
		size_t block_index = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
		const float *power = nullptr;
		if (block_index == last_block_index) {
			power = last_power;
		}
		else {
			power = analysis_cache->Get(block_index);
			last_block_index = block_index;
			last_power = power;
		}
		power_columns[static_cast<size_t>(ax - start)] = power;
	}

	if (last_block_index != static_cast<size_t>(-1))
		analysis_cache->Prefetch(last_block_index + 1, last_block_index + 8);

	RenderSpectrumColumnsToBitmap(
		bmp,
		power_columns,
		derivation_size,
		render_band_a.data(),
		render_band_b.data(),
		interpolated ? render_band_frac.data() : nullptr,
		interpolated,
		amplitude_scale,
		*pal);
}

void AudioSpectrumRenderer::RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style)
{
	// Get the colour of silence
	wxColour col = colors[style].get(0.0f);
	dc.SetBrush(wxBrush(col));
	dc.SetPen(wxPen(col));
	dc.DrawRectangle(rect);
}

void AudioSpectrumRenderer::AgeCache(size_t max_size)
{
	if (analysis_cache)
		analysis_cache->Age(max_size);
}

void AudioSpectrumRenderer::SetInteractivePrefetchEnabled(bool enabled) {
	if (analysis_cache)
		analysis_cache->SetPrefetchEnabled(enabled);
}

std::vector<std::string> AudioSpectrumRenderer::GetDebugInfo() const {
	if (!analysis_cache)
		return {};
	auto metrics = analysis_cache->GetMetricsSnapshot();
	std::ostringstream line1;
	std::ostringstream line2;
	line1 << "SP gen=" << metrics.generation
		<< " hits=" << metrics.cache_hits
		<< " miss=" << metrics.cache_misses
		<< " mode=" << static_cast<int>(computation_mode)
		<< " curve=" << frequency_curve_preset
		<< " vis=" << metrics.visible_builds
		<< " lock=" << metrics.visible_lock_contention
		<< " pf_req=" << metrics.prefetch_requests
		<< " pf_build=" << metrics.prefetch_builds
		<< " pf_skip=" << metrics.prefetch_busy_skips
		<< " pf_on=" << (metrics.prefetch_enabled ? 1 : 0)
		<< " stale=" << metrics.stale_drops;
	line2 << "SP cache entries=" << metrics.cache_entries
		<< " bytes=" << metrics.cache_bytes
		<< " evict=" << metrics.evictions;
	return {
		line1.str(),
		line2.str()
	};
}
