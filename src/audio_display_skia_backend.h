// Copyright (c) 2026
// All rights reserved.

#pragma once

#include <memory>

#include <wx/gdicmn.h>

class wxBitmap;
class wxWindow;
class AudioDisplaySkiaTarget;
class AudioDisplaySkiaPresentTarget;

enum class AudioDisplaySkiaBackendType {
	Bitmap = 0,
	ExperimentalGpu,
	DirectGpu,
};

class AudioDisplaySkiaBackend {
public:
	virtual ~AudioDisplaySkiaBackend() = default;

	virtual AudioDisplaySkiaBackendType GetBackendType() const = 0;
	virtual std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) = 0;
	virtual std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) = 0;
};

std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaBitmapBackend();
std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaExperimentalGpuBackend(wxWindow *owner);

class wxGLCanvas;
class wxGLContext;
std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaDirectGpuBackend(wxGLCanvas *canvas, wxGLContext *gl_ctx);
