#include "audio_waveform_bitmap_tile_renderer.h"

#include "compat.h"
#include "audio_waveform_summary_cache.h"
#include "audio_colorscheme.h"

#include <algorithm>
#include <wx/dcmemory.h>

namespace {
void DrawWaveformSummaryColumn(
	wxMemoryDC &dc,
	int x,
	int midpoint,
	const AudioWaveformSummary &summary,
	const wxPen &pen_peaks,
	const wxPen &pen_avgs,
	bool render_averages,
	float amplitude_scale)
{
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

template <typename SummaryLookup>
void RenderWaveformSummaries(
	wxBitmap &bmp,
	SummaryLookup &&lookup,
	const AudioColorScheme &palette,
	bool render_averages,
	float amplitude_scale)
{
	wxMemoryDC dc(bmp);
	wxRect rect(wxPoint(0, 0), bmp.GetSize());
	int midpoint = rect.height / 2;

	dc.SetBrush(wxBrush(to_wx(palette.get(0.0f))));
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(rect);

	wxPen pen_peaks(wxPen(to_wx(palette.get(0.4f))));
	wxPen pen_avgs(wxPen(to_wx(palette.get(0.7f))));

	for (int x = 0; x < rect.width; ++x) {
		const auto *summary = lookup(x);
		if (!summary)
			continue;

		DrawWaveformSummaryColumn(dc, x, midpoint, *summary, pen_peaks, pen_avgs, render_averages, amplitude_scale);
	}

	if (render_averages)
		dc.SetPen(wxPen(to_wx(palette.get(1.0f))));
	else
		dc.SetPen(pen_peaks);

	dc.DrawLine(0, midpoint, rect.width, midpoint);
}
}

void RenderWaveformSummaryColumnsToBitmap(
	wxBitmap &bmp,
	const std::vector<const AudioWaveformSummary *> &summaries,
	const AudioColorScheme &palette,
	bool render_averages,
	float amplitude_scale)
{
	RenderWaveformSummaries(
		bmp,
		[&summaries](int x) -> const AudioWaveformSummary * {
			if (x < 0 || static_cast<size_t>(x) >= summaries.size())
				return nullptr;
			return summaries[static_cast<size_t>(x)];
		},
		palette,
		render_averages,
		amplitude_scale);
}

void RenderWaveformSummaryBlockToBitmap(
	wxBitmap &bmp,
	const AudioWaveformSummaryBlock &block,
	const AudioColorScheme &palette,
	bool render_averages,
	float amplitude_scale)
{
	RenderWaveformSummaries(
		bmp,
		[&block](int x) -> const AudioWaveformSummary * {
			if (x < 0 || x >= static_cast<int>(AudioWaveformSummaryBlock::width))
				return nullptr;
			return &block.summaries[static_cast<size_t>(x)];
		},
		palette,
		render_averages,
		amplitude_scale);
}
