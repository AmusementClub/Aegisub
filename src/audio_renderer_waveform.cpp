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

#include "audio_display_source.h"
#include "audio_waveform_summary_cache.h"
#include "audio_colorscheme.h"
#include "options.h"

#include <libaegisub/make_unique.h>

#include <algorithm>
#include <sstream>
#include <wx/dcmemory.h>

enum {
	/// Only render the peaks
	Waveform_MaxOnly = 0,
	/// Render the peaks and averages
	Waveform_MaxAvg,
	Waveform_Continuous
};

AudioWaveformRenderer::AudioWaveformRenderer(std::string const& color_scheme_name)
: render_averages(OPT_GET("Audio/Display/Waveform Style")->GetInt() == Waveform_MaxAvg)
, summary_cache(agi::make_unique<AudioWaveformSummaryCache>())
{
	colors.reserve(AudioStyle_MAX);
	for (int i = 0; i < AudioStyle_MAX; ++i)
		colors.emplace_back(6, color_scheme_name, i);
}

AudioWaveformRenderer::~AudioWaveformRenderer() { }

void AudioWaveformRenderer::OnSetProvider() {
	if (summary_cache)
		summary_cache->SetSource(display_source);
}

void AudioWaveformRenderer::OnSetMillisecondsPerPixel() {
	if (summary_cache)
		summary_cache->SetMillisecondsPerPixel(pixel_ms);
}

void AudioWaveformRenderer::AgeCache(size_t max_size) {
	if (summary_cache)
		summary_cache->Age(max_size);
}

std::vector<std::string> AudioWaveformRenderer::GetDebugInfo() const {
	if (!summary_cache)
		return {};
	auto metrics = summary_cache->GetMetricsSnapshot();
	std::ostringstream line1;
	std::ostringstream line2;
	line1 << "WF gen=" << metrics.generation
		<< " hits=" << metrics.cache_hits
		<< " miss=" << metrics.cache_misses
		<< " vis=" << metrics.visible_builds
		<< " pf_req=" << metrics.prefetch_requests
		<< " pf_build=" << metrics.prefetch_builds
		<< " stale=" << metrics.stale_drops;
	line2 << "WF cache entries=" << metrics.cache_entries
		<< " bytes=" << metrics.cache_bytes
		<< " evict=" << metrics.evictions;
	return {
		line1.str(),
		line2.str()
	};
}

void AudioWaveformRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style)
{
	wxMemoryDC dc(bmp);
	wxRect rect(wxPoint(0, 0), bmp.GetSize());
	int midpoint = rect.height / 2;

	const AudioColorScheme *pal = &colors[style];

	if (!display_source || !summary_cache)
		return;

	summary_cache->SetSource(display_source);
	summary_cache->SetMillisecondsPerPixel(pixel_ms);
	summary_cache->SetMixPolicy(mix_policy);
	if (!summary_cache->IsReady())
		return;

	const auto &summary_block = summary_cache->Get(static_cast<size_t>(start / AudioWaveformSummaryBlock::width));

	// Fill the background
	dc.SetBrush(wxBrush(pal->get(0.0f)));
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(rect);

	wxPen pen_peaks(wxPen(pal->get(0.4f)));
	wxPen pen_avgs(wxPen(pal->get(0.7f)));

	for (int x = 0; x < rect.width && x < static_cast<int>(AudioWaveformSummaryBlock::width); ++x)
	{
		const auto &summary = summary_block.summaries[x];

		// midpoint is half height
		int peak_min = std::max(static_cast<int>(summary.peak_min * amplitude_scale * midpoint), -midpoint);
		int peak_max = std::min(static_cast<int>(summary.peak_max * amplitude_scale * midpoint), midpoint);
		int avg_min = std::max(static_cast<int>(summary.avg_min * amplitude_scale * midpoint), -midpoint);
		int avg_max = std::min(static_cast<int>(summary.avg_max * amplitude_scale * midpoint), midpoint);

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
