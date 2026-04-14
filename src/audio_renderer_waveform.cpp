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

#include "compat.h"
#include "audio_display_source.h"
#include "audio_waveform_bitmap_tile_renderer.h"
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
	summary_cache->SetReadyCallback([this] { NotifyRenderContentReady(); });
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

void AudioWaveformRenderer::SetInteractivePrefetchEnabled(bool enabled) {
	interactive_prefetch_enabled = enabled;
	if (summary_cache)
		summary_cache->SetPrefetchEnabled(enabled);
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

bool AudioWaveformRenderer::EnsureSummaryCacheConfigured() {
	if (!display_source || !summary_cache)
		return false;

	summary_cache->SetSource(display_source);
	summary_cache->SetMillisecondsPerPixel(pixel_ms);
	summary_cache->SetMixPolicy(mix_policy);
	summary_cache->SetPrefetchEnabled(interactive_prefetch_enabled);
	return summary_cache->IsReady();
}

std::pair<size_t, size_t> AudioWaveformRenderer::GetBlockRange(int start, int length) const {
	const size_t first_block = static_cast<size_t>(std::max(start, 0) / static_cast<int>(AudioWaveformSummaryBlock::width));
	const int end = start + std::max(length, 1) - 1;
	const size_t last_block = static_cast<size_t>(std::max(end, start) / static_cast<int>(AudioWaveformSummaryBlock::width));
	return { first_block, last_block };
}

void AudioWaveformRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style)
{
	if (!EnsureSummaryCacheConfigured()) {
		wxMemoryDC dc(bmp);
		RenderBlank(dc, wxRect(0, 0, bmp.GetWidth(), bmp.GetHeight()), style);
		return;
	}

	const size_t block_index = static_cast<size_t>(start / AudioWaveformSummaryBlock::width);
	const auto &summary_block = summary_cache->Get(block_index);
	if (interactive_prefetch_enabled)
		summary_cache->Prefetch(block_index + 1, block_index + 2);

	RenderWaveformSummaryBlockToBitmap(bmp, summary_block, colors[style], render_averages, amplitude_scale);
}

void AudioWaveformRenderer::WarmCacheRange(int start, int length) {
	if (!interactive_prefetch_enabled || !EnsureSummaryCacheConfigured() || length <= 0)
		return;

	auto const [first_block, last_block] = GetBlockRange(start, length);
	summary_cache->Prefetch(first_block, last_block + 2);
}

bool AudioWaveformRenderer::IsCacheRangeReady(int start, int length) {
	if (!EnsureSummaryCacheConfigured() || length <= 0)
		return true;

	auto const [first_block, last_block] = GetBlockRange(start, length);
	return summary_cache->AreBlocksReady(first_block, last_block);
}

void AudioWaveformRenderer::RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style)
{
	const AudioColorScheme *pal = &colors[style];
	wxColor line(to_wx(pal->get(1.0)));
	wxColor bg(to_wx(pal->get(0.0)));

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
