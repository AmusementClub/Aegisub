// Copyright (c) 2026
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#include <wx/gdicmn.h>

#ifdef WITH_SKIA
#include <include/core/SkRefCnt.h>
#endif

class wxBitmap;
class AudioDisplaySkiaTarget;
class AudioDisplaySkiaPresentTarget;
#ifdef WITH_SKIA
class SkSurface;
#endif

enum class AudioDisplaySkiaBackendType {
	Bitmap = 0,
	Gpu,
};

struct AudioDisplaySkiaGpuDiagnostics {
	bool available = false;
	bool software_like = false;
	std::string vendor;
	std::string renderer;
	std::string version;
	std::string shading_language_version;
};

class AudioDisplaySkiaBackend {
public:
	virtual ~AudioDisplaySkiaBackend() = default;

	virtual AudioDisplaySkiaBackendType GetBackendType() const = 0;
	virtual std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) = 0;
	virtual std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) = 0;
	virtual bool QueryGpuDiagnostics(AudioDisplaySkiaGpuDiagnostics &out) = 0;
#ifdef WITH_SKIA
	virtual sk_sp<SkSurface> AcquireCachedContentSurface(int width, int height) = 0;
	virtual sk_sp<SkSurface> AcquireCachedFrameSurface(int width, int height) = 0;
#endif
};

int *GetAudioDisplayGlAttribs();
std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaBitmapBackend();

class wxGLCanvas;
class wxGLContext;
std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaGpuBackend(wxGLCanvas *canvas, wxGLContext *gl_ctx);
