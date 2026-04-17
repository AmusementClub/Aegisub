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

#include "compat.h"
#include "audio_display_analysis.h"
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
#include <wx/string.h>

namespace {
std::string GetChannelLabel(int channel, int total_channels) {
	if (total_channels == 1)
		return "M";
	if (total_channels == 2)
		return channel == 0 ? "L" : "R";
	if (total_channels == 6) {
		static const char *labels[] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
		if (channel >= 0 && channel < 6)
			return labels[channel];
	}
	if (total_channels == 8) {
		static const char *labels[] = {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"};
		if (channel >= 0 && channel < 8)
			return labels[channel];
	}
	return "CH" + std::to_string(channel + 1);
}
}

AudioSpectrumRenderer::AudioSpectrumRenderer(std::string const& color_scheme_name)
{
	colors.reserve(AudioStyle_MAX);
	for (int i = 0; i < AudioStyle_MAX; ++i)
		colors.emplace_back(12, color_scheme_name, i);
	analysis_cache = std::make_unique<AudioSpectrumAnalysisCache>();
	analysis_cache->SetReadyCallback([this] { NotifyRenderContentReady(); });
	ConfigurePrefetchBudgets();
}

void AudioSpectrumRenderer::OnAllowPlaceholderChanged() {
	ConfigurePrefetchBudgets();
}

bool AudioSpectrumRenderer::UsesAnalysisCache() const {
	return channel_mode == AudioSpectrumChannelMode::MonoMix
		&& mono_mix_mode == AudioSpectrumMonoMixMode::MonoAverage;
}

bool AudioSpectrumRenderer::UsesPerChannelCaches() const {
	if (!display_source)
		return false;
	const int total_channels = std::max(1, display_source->GetChannels());
	if (total_channels <= 1)
		return false;
	return channel_mode == AudioSpectrumChannelMode::ChannelSplit
		|| mono_mix_mode != AudioSpectrumMonoMixMode::MonoAverage;
}

bool AudioSpectrumRenderer::UsesPerChannelMonoAggregation() const {
	return channel_mode == AudioSpectrumChannelMode::MonoMix
		&& mono_mix_mode != AudioSpectrumMonoMixMode::MonoAverage
		&& UsesPerChannelCaches();
}

void AudioSpectrumRenderer::EnsurePerChannelCaches() {
	per_channel_sources.clear();
	per_channel_caches.clear();
	active_channel_indices.clear();
	active_channel_labels.clear();

	if (!UsesPerChannelCaches())
		return;

	const int total_channels = std::max(1, display_source->GetChannels());
	std::vector<int> channels_to_use;
	if (channel_mode != AudioSpectrumChannelMode::ChannelSplit || selected_channels.empty()) {
		channels_to_use.reserve(total_channels);
		for (int ch = 0; ch < total_channels; ++ch)
			channels_to_use.push_back(ch);
	}
	else {
		channels_to_use = selected_channels;
		channels_to_use.erase(std::remove_if(channels_to_use.begin(), channels_to_use.end(),
			[total_channels](int ch) { return ch < 0 || ch >= total_channels; }), channels_to_use.end());
		std::sort(channels_to_use.begin(), channels_to_use.end());
		channels_to_use.erase(std::unique(channels_to_use.begin(), channels_to_use.end()), channels_to_use.end());
		if (channels_to_use.empty()) {
			for (int ch = 0; ch < total_channels; ++ch)
				channels_to_use.push_back(ch);
		}
	}

	for (int ch : channels_to_use) {
		per_channel_sources.push_back(CreateSingleChannelAudioDisplaySource(display_source, ch));
		auto cache = std::make_unique<AudioSpectrumAnalysisCache>();
		cache->SetSource(per_channel_sources.back().get());
		cache->SetMixPolicy(mix_policy);
		cache->SetResolution(derivation_size, derivation_dist);
		cache->SetPrefetchEnabled(interactive_prefetch_enabled);
		cache->SetReadyCallback([this] { NotifyRenderContentReady(); });
		per_channel_caches.push_back(std::move(cache));
		active_channel_indices.push_back(ch);
		active_channel_labels.push_back(GetChannelLabel(ch, total_channels));
	}

	ConfigurePrefetchBudgets();
}

void AudioSpectrumRenderer::ConfigurePrefetchBudgets() {
	const size_t max_blocks = allow_placeholder ? size_t{256} : size_t{1024};
	if (analysis_cache)
		analysis_cache->SetPrefetchBuildMaxBlocks(max_blocks);
	for (auto &cache : per_channel_caches) {
		if (cache)
			cache->SetPrefetchBuildMaxBlocks(max_blocks);
	}
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
		if (UsesAnalysisCache()) {
			analysis_cache->SetSource(display_source);
			analysis_cache->SetMixPolicy(mix_policy);
			analysis_cache->SetResolution(derivation_size, derivation_dist);
			analysis_cache->SetPrefetchEnabled(interactive_prefetch_enabled);
		}
		else {
			analysis_cache->SetSource(nullptr);
		}
	}
	for (auto &cache : per_channel_caches) {
		if (cache) {
			cache->SetMixPolicy(mix_policy);
			cache->SetResolution(derivation_size, derivation_dist);
			cache->SetPrefetchEnabled(interactive_prefetch_enabled);
		}
	}
}

void AudioSpectrumRenderer::OnSetProvider()
{
	RecreateCache();
	EnsurePerChannelCaches();
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

void AudioSpectrumRenderer::SetChannelMode(AudioSpectrumChannelMode mode) {
	if (channel_mode == mode)
		return;
	channel_mode = mode;
	RecreateCache();
	EnsurePerChannelCaches();
	render_scale_cache_height = 0;
	AgeCache(0);
}

void AudioSpectrumRenderer::SetMonoMixMode(AudioSpectrumMonoMixMode mode) {
	if (mono_mix_mode == mode)
		return;
	const bool old_uses_analysis = UsesAnalysisCache();
	const bool old_uses_per_channel = UsesPerChannelCaches();
	mono_mix_mode = mode;
	const bool new_uses_analysis = UsesAnalysisCache();
	const bool new_uses_per_channel = UsesPerChannelCaches();
	if (old_uses_analysis != new_uses_analysis || old_uses_per_channel != new_uses_per_channel) {
		RecreateCache();
		EnsurePerChannelCaches();
		AgeCache(0);
	}
}

void AudioSpectrumRenderer::SetSelectedChannels(const std::vector<int> &channels) {
	selected_channels = channels;
	EnsurePerChannelCaches();
	render_scale_cache_height = 0;
	AgeCache(0);
}

bool AudioSpectrumRenderer::EnsureCachesConfigured() {
	if (!display_source)
		return false;
	if (UsesPerChannelCaches() && per_channel_caches.empty())
		EnsurePerChannelCaches();
	return true;
}

std::pair<size_t, size_t> AudioSpectrumRenderer::GetVisibleBlockRange(int start, int length) const {
	const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
	const int end = start + std::max(length, 1);
	const size_t first_visible_block = static_cast<size_t>(std::max(start, 0) * samples_per_pixel) >> derivation_dist;
	const size_t last_visible_block = static_cast<size_t>(std::max(start, end - 1) * samples_per_pixel) >> derivation_dist;
	return { first_visible_block, last_visible_block };
}

void AudioSpectrumRenderer::WarmCacheRange(int start, int length) {
	if (!interactive_prefetch_enabled || !EnsureCachesConfigured() || length <= 0)
		return;

	auto const [first_visible_block, last_visible_block] = GetVisibleBlockRange(start, length);
	if (UsesPerChannelCaches() && !per_channel_caches.empty()) {
		for (auto &cache : per_channel_caches) {
			if (cache && cache->IsReady())
				cache->Prefetch(first_visible_block, last_visible_block + 8);
		}
		return;
	}

	if (analysis_cache && analysis_cache->IsReady())
		analysis_cache->Prefetch(first_visible_block, last_visible_block + 8);
}

bool AudioSpectrumRenderer::IsCacheRangeReady(int start, int length) {
	if (!EnsureCachesConfigured() || length <= 0)
		return true;

	auto const [first_visible_block, last_visible_block] = GetVisibleBlockRange(start, length);
	if (UsesPerChannelCaches() && !per_channel_caches.empty()) {
		for (auto &cache : per_channel_caches) {
			if (!cache || !cache->IsReady())
				return true;
		}

		for (auto &cache : per_channel_caches) {
			if (!cache->AreBlocksReady(first_visible_block, last_visible_block))
				return false;
		}
		return true;
	}

	if (!analysis_cache || !analysis_cache->IsReady())
		return true;

	return analysis_cache->AreBlocksReady(first_visible_block, last_visible_block);
}

AudioRenderResult AudioSpectrumRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style)
{
	if (!EnsureCachesConfigured() || bmp.GetWidth() <= 0) {
		wxMemoryDC dc(bmp);
		RenderBlank(dc, wxRect(0, 0, bmp.GetWidth(), bmp.GetHeight()), style);
		return AudioRenderResult::Ready;
	}

	// ChannelSplit path: render each channel into its own horizontal band
	if (channel_mode == AudioSpectrumChannelMode::ChannelSplit && !per_channel_caches.empty()) {
		const int channels = static_cast<int>(per_channel_caches.size());
		const int total_height = bmp.GetHeight();
		const int band_height = total_height / channels;

		if (band_height >= 4 && bmp.GetWidth() > 0) {
			const bool caches_ready = std::all_of(per_channel_caches.begin(), per_channel_caches.end(),
				[](const auto &cache) { return cache && cache->IsReady(); });
			if (!caches_ready) {
				wxMemoryDC dc(bmp);
				RenderBlank(dc, wxRect(0, 0, bmp.GetWidth(), bmp.GetHeight()), style);
				return AudioRenderResult::Ready;
			}

			const AudioColorScheme *pal = &colors[style];
			const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
			const int end = start + bmp.GetWidth();
			EnsureRenderScaleCache(band_height);
			const bool ch_interpolated = band_height > 1 << derivation_size;
			const size_t first_visible_block = static_cast<size_t>(start * samples_per_pixel) >> derivation_dist;
			const size_t last_visible_block = static_cast<size_t>(std::max(start, end - 1) * samples_per_pixel) >> derivation_dist;

			for (int ch = 0; ch < channels; ++ch)
				per_channel_caches[ch]->Prefetch(first_visible_block, last_visible_block + 8);

			channel_split_power_columns_scratch.resize(static_cast<size_t>(bmp.GetWidth()));
			const bool needs_recreate = !channel_split_band_bitmap_scratch.IsOk()
				|| channel_split_band_bitmap_scratch.GetWidth() != bmp.GetWidth()
				|| channel_split_band_bitmap_scratch.GetHeight() != band_height;
			if (needs_recreate)
				channel_split_band_bitmap_scratch = wxBitmap(bmp.GetWidth(), band_height);

			wxMemoryDC dst_dc(bmp);
			bool has_missing_columns = false;
			for (int ch = 0; ch < channels; ++ch) {
				size_t last_block = static_cast<size_t>(-1);
				const float *last_power = nullptr;

				for (int ax = start; ax < end; ++ax) {
					size_t block_idx = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
					const float *power = nullptr;
					if (block_idx == last_block) {
						power = last_power;
					}
					else {
						if (allow_placeholder)
							power = per_channel_caches[ch]->GetIfReady(block_idx);
						else
							power = per_channel_caches[ch]->Get(block_idx);
						last_block = block_idx;
						last_power = power;
					}
					if (!power)
						has_missing_columns = true;
					channel_split_power_columns_scratch[static_cast<size_t>(ax - start)] = power;
				}

				RenderSpectrumColumnsToBitmap(
					channel_split_band_bitmap_scratch, channel_split_power_columns_scratch, derivation_size,
					render_band_a.data(), render_band_b.data(),
					ch_interpolated ? render_band_frac.data() : nullptr,
					ch_interpolated, amplitude_scale, *pal);

				dst_dc.DrawBitmap(channel_split_band_bitmap_scratch, 0, ch * band_height);
			}

			// Draw dividers between channel bands
			if (channels > 1) {
				dst_dc.SetPen(wxPen(wxColour(80, 80, 80), 1));
				for (int ch = 1; ch < channels; ++ch)
					dst_dc.DrawLine(0, ch * band_height, bmp.GetWidth(), ch * band_height);
			}

			return has_missing_columns ? AudioRenderResult::Placeholder : AudioRenderResult::Ready;
		}
	}

	if (UsesPerChannelMonoAggregation() && !per_channel_caches.empty()) {
		const bool caches_ready = std::all_of(per_channel_caches.begin(), per_channel_caches.end(),
			[](const auto &cache) { return cache && cache->IsReady(); });
		if (!caches_ready) {
			wxMemoryDC dc(bmp);
			RenderBlank(dc, wxRect(0, 0, bmp.GetWidth(), bmp.GetHeight()), style);
			return AudioRenderResult::Ready;
		}

		assert(bmp.IsOk());

		const int end = start + bmp.GetWidth();
		const AudioColorScheme *pal = &colors[style];
		const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
		const size_t first_visible_block = static_cast<size_t>(start * samples_per_pixel) >> derivation_dist;
		const size_t last_visible_block = static_cast<size_t>(std::max(start, end - 1) * samples_per_pixel) >> derivation_dist;
		EnsureRenderScaleCache(bmp.GetHeight());
		const bool interpolated = bmp.GetHeight() > 1 << derivation_size;
		const size_t bin_count = static_cast<size_t>(1) << derivation_size;

		combined_power_columns.resize(static_cast<size_t>(bmp.GetWidth()));
		combined_power_scratch.resize(static_cast<size_t>(bmp.GetWidth()) * bin_count);
		channel_power_inputs.resize(per_channel_caches.size());
		for (auto &cache : per_channel_caches)
			cache->Prefetch(first_visible_block, last_visible_block + 8);

		size_t last_block_index = static_cast<size_t>(-1);
		const float *last_power = nullptr;
		size_t combined_block_count = 0;
		bool has_missing_columns = false;

		for (int ax = start; ax < end; ++ax) {
			size_t block_index = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
			const float *power = nullptr;
			if (block_index == last_block_index) {
				power = last_power;
			}
			else {
				bool ready = true;
				for (size_t ch = 0; ch < per_channel_caches.size(); ++ch) {
					if (allow_placeholder)
						channel_power_inputs[ch] = per_channel_caches[ch]->GetIfReady(block_index);
					else
						channel_power_inputs[ch] = per_channel_caches[ch]->Get(block_index);
					if (!channel_power_inputs[ch])
						ready = false;
				}

				if (!ready) {
					has_missing_columns = true;
					power = nullptr;
				}
				else {
					float *dst = combined_power_scratch.data() + combined_block_count * bin_count;
					if (mono_mix_mode == AudioSpectrumMonoMixMode::PerBinMaxPower)
						MergeSpectrumPowerBinsMax(channel_power_inputs, bin_count, dst);
					else
						MergeSpectrumPowerBinsAverage(channel_power_inputs, bin_count, dst);
					power = dst;
					++combined_block_count;
				}
				last_block_index = block_index;
				last_power = power;
			}
			combined_power_columns[static_cast<size_t>(ax - start)] = power;
		}

		RenderSpectrumColumnsToBitmap(
			bmp,
			combined_power_columns,
			derivation_size,
			render_band_a.data(),
			render_band_b.data(),
			interpolated ? render_band_frac.data() : nullptr,
			interpolated,
			amplitude_scale,
			*pal);
		return has_missing_columns ? AudioRenderResult::Placeholder : AudioRenderResult::Ready;
	}

	if (!analysis_cache || !analysis_cache->IsReady()) {
		wxMemoryDC dc(bmp);
		RenderBlank(dc, wxRect(0, 0, bmp.GetWidth(), bmp.GetHeight()), style);
		return AudioRenderResult::Ready;
	}

	assert(bmp.IsOk());

	int end = start + bmp.GetWidth();

	assert(start >= 0);
	assert(end >= start);

	const AudioColorScheme *pal = &colors[style];
	const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
	const size_t first_visible_block = static_cast<size_t>(start * samples_per_pixel) >> derivation_dist;
	const size_t last_visible_block = static_cast<size_t>(std::max(start, end - 1) * samples_per_pixel) >> derivation_dist;
	EnsureRenderScaleCache(bmp.GetHeight());
	const bool interpolated = bmp.GetHeight() > 1 << derivation_size;
	analysis_cache->Prefetch(first_visible_block, last_visible_block + 8);

	power_columns_scratch.resize(static_cast<size_t>(bmp.GetWidth()));
	size_t last_block_index = static_cast<size_t>(-1);
	const float *last_power = nullptr;
	bool has_missing_columns = false;

	for (int ax = start; ax < end; ++ax) {
		size_t block_index = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
		const float *power = nullptr;
		if (block_index == last_block_index) {
			power = last_power;
		}
		else {
			if (allow_placeholder)
				power = analysis_cache->GetIfReady(block_index);
			else
				power = analysis_cache->Get(block_index);
			last_block_index = block_index;
			last_power = power;
		}
		if (!power)
			has_missing_columns = true;
		power_columns_scratch[static_cast<size_t>(ax - start)] = power;
	}

	RenderSpectrumColumnsToBitmap(
		bmp,
		power_columns_scratch,
		derivation_size,
		render_band_a.data(),
		render_band_b.data(),
		interpolated ? render_band_frac.data() : nullptr,
		interpolated,
		amplitude_scale,
		*pal);
	return has_missing_columns ? AudioRenderResult::Placeholder : AudioRenderResult::Ready;
}

void AudioSpectrumRenderer::RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style)
{
	// Get the colour of silence
	wxColour col = to_wx(colors[style].get(0.0f));
	dc.SetBrush(wxBrush(col));
	dc.SetPen(wxPen(col));
	dc.DrawRectangle(rect);
}

void AudioSpectrumRenderer::AgeCache(size_t max_size)
{
	if (UsesPerChannelCaches() && !per_channel_caches.empty()) {
		if (analysis_cache)
			analysis_cache->Age(0);
		const size_t n = per_channel_caches.size();
		const size_t ch_max = n > 0 ? max_size / n : 0;
		for (auto &cache : per_channel_caches)
			cache->Age(ch_max);
	}
	else {
		if (analysis_cache)
			analysis_cache->Age(max_size);
		for (auto &cache : per_channel_caches)
			cache->Age(0);
	}
}

void AudioSpectrumRenderer::SetInteractivePrefetchEnabled(bool enabled) {
	interactive_prefetch_enabled = enabled;
	if (analysis_cache)
		analysis_cache->SetPrefetchEnabled(enabled);
	for (auto &cache : per_channel_caches)
		cache->SetPrefetchEnabled(enabled);
}

std::vector<std::string> AudioSpectrumRenderer::GetDebugInfo() const {
	const AudioSpectrumAnalysisCache *metrics_cache = analysis_cache.get();
	if (UsesPerChannelCaches() && !per_channel_caches.empty() && per_channel_caches.front())
		metrics_cache = per_channel_caches.front().get();
	if (!metrics_cache)
		return {};
	auto metrics = metrics_cache->GetMetricsSnapshot();
	std::ostringstream line1;
	std::ostringstream line2;
	line1 << "SP gen=" << metrics.generation
		<< " hits=" << metrics.cache_hits
		<< " miss=" << metrics.cache_misses
		<< " mode=" << static_cast<int>(computation_mode)
		<< " curve=" << frequency_curve_preset
		<< " ch=" << static_cast<int>(channel_mode)
		<< " mono=" << static_cast<int>(mono_mix_mode)
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
