// Copyright (c) 2010, Niels Martin Hansen
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

#include "audio_renderer_waveform.h"

#include "audio_colorscheme.h"
#include "audio_display_source.h"
#include "audio_waveform_column_ref.h"
#include "audio_waveform_summary_cache.h"
#include "options.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <limits>
#include <wx/dcmemory.h>

enum {
	/// Only render the peaks
	Waveform_MaxOnly = 0,
	/// Render the peaks and averages
	Waveform_MaxAvg,
	Waveform_Continuous
};

AudioWaveformRenderer::AudioWaveformRenderer(std::string const& color_scheme_name)
: summary_cache(std::make_unique<AudioWaveformSummaryCache>())
, render_averages(OPT_GET("Audio/Display/Waveform Style")->GetInt() == Waveform_MaxAvg)
{
	colors.reserve(AudioStyle_MAX);
	for (int i = 0; i < AudioStyle_MAX; ++i)
		colors.emplace_back(6, color_scheme_name, i);
}

AudioWaveformRenderer::~AudioWaveformRenderer() { }

void AudioWaveformRenderer::OnSetProvider() {
	summary_cache->SetSource(nullptr);
	display_source = CreateInt16MonoAudioDisplaySource(provider);
	summary_cache->SetSource(display_source.get());
}

void AudioWaveformRenderer::OnSetMillisecondsPerPixel() {
	summary_cache->SetMillisecondsPerPixel(pixel_ms);
}

void AudioWaveformRenderer::AgeCache(size_t max_size) {
	summary_cache->Age(max_size);
}

void AudioWaveformRenderer::Prefetch(int start, int length) {
	if (!display_source || !provider)
		return;

	auto const range = PlanWaveformPrefetchBlocks(
		start,
		length,
		provider->GetDecodedSamples(),
		provider->GetSampleRate(),
		pixel_ms);
	if (range)
		summary_cache->Prefetch(range->first, range->last);
}

bool AudioWaveformRenderer::GetCacheMetrics(AudioRendererCacheMetrics &metrics) const {
	if (!summary_cache)
		return false;
	auto const source = summary_cache->GetMetricsSnapshot();
	metrics.content_kind = AudioRendererContentKind::Waveform;
	metrics.generation = source.generation;
	metrics.source_cache_hits = source.cache_hits;
	metrics.source_cache_misses = source.cache_misses;
	metrics.visible_builds = source.visible_builds;
	metrics.visible_lock_contention = source.visible_lock_contention;
	metrics.prefetch_requests = source.prefetch_requests;
	metrics.prefetch_builds = source.prefetch_builds;
	metrics.prefetch_busy_skips = source.prefetch_busy_skips;
	metrics.stale_drops = source.stale_drops;
	metrics.evictions = source.evictions;
	metrics.cache_entries = source.cache_entries;
	metrics.cache_bytes = source.cache_bytes;
	metrics.cache_budget_bytes = source.cache_budget_bytes;
	metrics.prefetch_enabled = source.prefetch_enabled;
	return true;
}

void AudioWaveformRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style)
{
	wxMemoryDC dc(bmp);
	wxRect rect(wxPoint(0, 0), bmp.GetSize());
	int midpoint = rect.height / 2;

	const AudioColorScheme *pal = &colors[style];

	double pixel_samples = pixel_ms * provider->GetSampleRate() / 1000.0;

	// Fill the background
	dc.SetBrush(wxBrush(pal->get(0.0f)));
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(rect);

	wxPen pen_peaks(wxPen(pal->get(0.4f)));
	wxPen pen_avgs(wxPen(pal->get(0.7f)));

	size_t active_block_index = std::numeric_limits<size_t>::max();
	AudioWaveformSummaryCache::BlockHandle active_block;
	for (int x = 0; x < rect.width; ++x)
	{
		const auto column_ref = GetWaveformSummaryColumnRef(start + x);
		if (column_ref.block_index != active_block_index) {
			active_block_index = column_ref.block_index;
			active_block = summary_cache->Get(active_block_index);
		}
		if (!active_block)
			continue;

		// midpoint is half height
		int peak_min = 0;
		int peak_max = 0;
		int avg_min = 0;
		int avg_max = 0;
		if (active_block->has_exact_pcm16) {
			const auto &summary = active_block->pcm16_summaries[column_ref.summary_index];
			peak_min = std::max((int)(summary.peak_min * amplitude_scale * midpoint) / 0x8000, -midpoint);
			peak_max = std::min((int)(summary.peak_max * amplitude_scale * midpoint) / 0x8000, midpoint);
			avg_min = std::max((int)(summary.avg_min_accum * amplitude_scale * midpoint / pixel_samples) / 0x8000, -midpoint);
			avg_max = std::min((int)(summary.avg_max_accum * amplitude_scale * midpoint / pixel_samples) / 0x8000, midpoint);
		}
		else {
			const auto &summary = active_block->summaries[column_ref.summary_index];
			peak_min = std::max(static_cast<int>(summary.peak_min * amplitude_scale * midpoint), -midpoint);
			peak_max = std::min(static_cast<int>(summary.peak_max * amplitude_scale * midpoint), midpoint);
			avg_min = std::max(static_cast<int>(summary.avg_min * amplitude_scale * midpoint), -midpoint);
			avg_max = std::min(static_cast<int>(summary.avg_max * amplitude_scale * midpoint), midpoint);
		}

		dc.SetPen(pen_peaks);
		dc.DrawLine(x, midpoint - peak_max, x, midpoint - peak_min);
		if (render_averages) {
			dc.SetPen(pen_avgs);
			dc.DrawLine(x, midpoint - avg_max, x, midpoint - avg_min);
		}
	}

	// Horizontal zero-point line
	if (render_averages)
		dc.SetPen(wxPen(pal->get(1.0f)));
	else
		dc.SetPen(pen_peaks);

	dc.DrawLine(0, midpoint, rect.width, midpoint);
}

void AudioWaveformRenderer::RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style)
{
	const AudioColorScheme *pal = &colors[style];
	wxColor line(pal->get(1.0));
	wxColor bg(pal->get(0.0));

	// Draw the line as background above and below, and line in the middle, to avoid
	// overdraw flicker (the common theme in all of audio display direct drawing).
	int halfheight = rect.height / 2;

	dc.SetBrush(wxBrush(bg));
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(rect.x, rect.y, rect.width, halfheight);
	dc.DrawRectangle(rect.x, rect.y + halfheight + 1, rect.width, rect.height - halfheight - 1);

	dc.SetPen(wxPen(line));
	dc.DrawLine(rect.x, rect.y+halfheight, rect.x+rect.width, rect.y+halfheight);
}

wxArrayString AudioWaveformRenderer::GetWaveformStyles() {
	wxArrayString ret;
	ret.push_back(_("Maximum"));
	ret.push_back(_("Maximum + Average"));
	return ret;
}
