// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "audio_display_render_model.h"

#include <wx/bitmap.h>

class SkCanvas;

class AudioDisplaySkiaRenderer {
public:
	bool CanDrawFrame(AudioDisplayRenderModel const& model) const;
	bool DrawFrameToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model, wxPoint origin) const;
	bool CanDrawContent(AudioDisplayRenderModel const& model) const;
	bool DrawContentToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model) const;
	bool CanDrawAudioAreaFrame(AudioDisplayRenderModel const& model) const;
	bool DrawAudioAreaFrameToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model, wxPoint origin) const;
	bool CanDrawAudioAreaOverlays(AudioDisplayRenderModel const& model) const;
	bool DrawAudioAreaOverlaysToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model, wxPoint origin) const;
#ifdef WITH_SKIA
	bool DrawFrameToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const;
	bool DrawContentToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const;
	bool DrawAudioAreaFrameToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const;
	bool CompositeAudioAreaOverlaysToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const;
	bool DrawAudioAreaOverlaysToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const;
	bool DrawChromeToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const;
#endif
	bool CanDrawWaveformContent(AudioDisplayRenderModel const& model) const;
	bool DrawWaveformContentToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model) const;
	bool CanDrawSpectrumContent(AudioDisplayRenderModel const& model) const;
	bool DrawSpectrumContentToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model) const;
};
