#pragma once

#include <wx/bitmap.h>

class AudioColorScheme;
struct AudioWaveformSummaryBlock;

void RenderWaveformSummaryBlockToBitmap(
	wxBitmap &bmp,
	const AudioWaveformSummaryBlock &block,
	const AudioColorScheme &palette,
	bool render_averages,
	float amplitude_scale);
