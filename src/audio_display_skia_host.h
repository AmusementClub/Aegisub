// Copyright (c) 2026
// All rights reserved.

#pragma once

#include <memory>

#include <wx/gdicmn.h>

class wxBitmap;
class wxWindow;
class AudioDisplaySkiaTarget;
class AudioDisplaySkiaPresentTarget;

enum class AudioDisplaySkiaHostBackend {
	Bitmap = 0,
	ExperimentalGpu,
	DirectGpu,
};

class AudioDisplaySkiaHost {
public:
	virtual ~AudioDisplaySkiaHost() = default;

	virtual AudioDisplaySkiaHostBackend GetBackend() const = 0;
	virtual std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) = 0;
	virtual std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) = 0;
};

std::unique_ptr<AudioDisplaySkiaHost> CreateAudioDisplaySkiaBitmapHost();
std::unique_ptr<AudioDisplaySkiaHost> CreateAudioDisplaySkiaExperimentalGpuHost(wxWindow *owner);

class wxGLCanvas;
class wxGLContext;
std::unique_ptr<AudioDisplaySkiaHost> CreateAudioDisplaySkiaDirectGpuHost(wxGLCanvas *canvas, wxGLContext *gl_ctx);
