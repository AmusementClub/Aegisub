// Copyright (c) 2026
// All rights reserved.

#include "audio_display_skia_host.h"

#include "audio_display_skia_target.h"
#include "skia_runtime/skia_gpu_context_host.h"

#ifdef WITH_SKIA
#include <include/core/SkColorSpace.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLTypes.h>
#endif

#include <wx/frame.h>
#include <wx/glcanvas.h>
#include <wx/window.h>

#include <memory>
#include <vector>

namespace {
#if wxCHECK_VERSION(3, 1, 1)
int audio_display_gl_attribs[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, WX_GL_BUFFER_SIZE, 24, WX_GL_MIN_ALPHA, 8, 0 };
#else
int audio_display_gl_attribs[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

class AudioDisplaySkiaBitmapHost final : public AudioDisplaySkiaHost {
public:
	AudioDisplaySkiaHostBackend GetBackend() const override {
		return AudioDisplaySkiaHostBackend::Bitmap;
	}

	std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) override {
		return CreateAudioDisplaySkiaBitmapTarget(bitmap, rect);
	}

	std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) override {
		return CreateAudioDisplaySkiaBitmapPresentTarget(rect);
	}
};

struct AudioDisplaySkiaOffscreenSurfaceCache {
	int width = 0;
	int height = 0;
#ifdef WITH_SKIA
	sk_sp<SkSurface> surface;
#endif
};

struct AudioDisplaySkiaOffscreenReadbackCache {
	int width = 0;
	int height = 0;
	wxBitmap bitmap;
	std::vector<uint32_t> pixels;
};

class AudioDisplaySkiaGpuOffscreenHost;

class AudioDisplaySkiaGpuOffscreenContentTarget final : public AudioDisplaySkiaTarget {
	AudioDisplaySkiaGpuOffscreenHost &host;
	wxRect rect;
	wxBitmap *dest_bitmap = nullptr;
	AudioDisplaySkiaOffscreenReadbackCache *readback = nullptr;
#ifdef WITH_SKIA
	sk_sp<SkSurface> surface;
#endif

public:
	AudioDisplaySkiaGpuOffscreenContentTarget(
		AudioDisplaySkiaGpuOffscreenHost &host,
		wxBitmap &bitmap,
		wxRect const& rect);
	bool IsValid() const override;
	wxRect const& GetRect() const override { return rect; }
	SkCanvas *GetCanvas() const override;
	bool Finalize() override;
};

class AudioDisplaySkiaGpuOffscreenPresentTarget final : public AudioDisplaySkiaPresentTarget {
	AudioDisplaySkiaGpuOffscreenHost &host;
	wxRect rect;
	AudioDisplaySkiaOffscreenReadbackCache *readback = nullptr;
#ifdef WITH_SKIA
	sk_sp<SkSurface> surface;
#endif

public:
	AudioDisplaySkiaGpuOffscreenPresentTarget(AudioDisplaySkiaGpuOffscreenHost &host, wxRect const& rect);
	bool IsValid() const override;
	wxRect const& GetRect() const override { return rect; }
	SkCanvas *GetCanvas() const override;
	bool Finalize() override;
	bool PresentTo(wxDC &dc, bool use_mask) override;
};

class AudioDisplaySkiaGpuOffscreenHost final : public AudioDisplaySkiaHost {
	wxWindow *owner = nullptr;
	wxFrame *hidden_frame = nullptr;
	wxGLCanvas *canvas = nullptr;
	std::unique_ptr<wxGLContext> gl_context;
	std::unique_ptr<SkiaGpuContextHost> context_host;
	AudioDisplaySkiaOffscreenSurfaceCache content_surface_cache;
	AudioDisplaySkiaOffscreenSurfaceCache present_surface_cache;
	AudioDisplaySkiaOffscreenReadbackCache content_readback_cache;
	AudioDisplaySkiaOffscreenReadbackCache present_readback_cache;

	bool EnsureCanvas() {
		if (canvas && gl_context)
			return true;
		if (!owner)
			return false;

		// Create the wxGLCanvas in a completely separate hidden top-level
		// frame.  This isolates the GL window from the AudioBox/AudioDisplay
		// window tree entirely, preventing any disruption of Windows Desktop
		// Window Manager compositing for sibling controls (sliders, toolbar
		// buttons).  A GL child window anywhere in the visible window tree
		// can alter the pixel format handling of the parent chain.
		hidden_frame = new wxFrame(nullptr, wxID_ANY, wxEmptyString,
			wxPoint(-32000, -32000), wxSize(1, 1),
			wxFRAME_NO_TASKBAR | wxFRAME_TOOL_WINDOW | wxBORDER_NONE);
		hidden_frame->Hide();
		canvas = new wxGLCanvas(hidden_frame, wxID_ANY, audio_display_gl_attribs, wxDefaultPosition, wxSize(1, 1), wxBORDER_NONE);
		canvas->Hide();
		gl_context = std::make_unique<wxGLContext>(canvas);
		if (!gl_context || !gl_context->IsOK()) {
			delete canvas;
			canvas = nullptr;
			gl_context.reset();
			hidden_frame->Destroy();
			hidden_frame = nullptr;
			return false;
		}
		context_host = std::make_unique<SkiaGpuContextHost>();
		return static_cast<bool>(canvas) && static_cast<bool>(gl_context) && static_cast<bool>(context_host);
	}

public:
	explicit AudioDisplaySkiaGpuOffscreenHost(wxWindow *owner)
	: owner(owner) {
	}

	~AudioDisplaySkiaGpuOffscreenHost() override {
		content_surface_cache.surface.reset();
		present_surface_cache.surface.reset();
		content_readback_cache.bitmap = wxBitmap();
		present_readback_cache.bitmap = wxBitmap();
		content_readback_cache.pixels.clear();
		present_readback_cache.pixels.clear();
		if (context_host)
			context_host->Reset();
		context_host.reset();
		gl_context.reset();
		if (canvas) {
			auto *old_canvas = canvas;
			canvas = nullptr;
			delete old_canvas;
		}
		if (hidden_frame) {
			hidden_frame->Destroy();
			hidden_frame = nullptr;
		}
	}

	bool EnsureCurrent() {
		if (!EnsureCanvas())
			return false;
		canvas->SetCurrent(*gl_context);
		return context_host && context_host->EnsureCurrentContext();
	}

#ifdef WITH_SKIA
	GrDirectContext *GetContext() const {
		return context_host ? context_host->Get() : nullptr;
	}

	void Flush() {
		if (context_host)
			context_host->FlushAndSubmit();
	}

	sk_sp<SkSurface> AcquireSurface(bool for_present, int width, int height) {
		if (!EnsureCurrent() || width <= 0 || height <= 0)
			return nullptr;

		auto &cache = for_present ? present_surface_cache : content_surface_cache;
		if (cache.surface && cache.width == width && cache.height == height)
			return cache.surface;

		auto const image_info = SkImageInfo::Make(
			width,
			height,
			kBGRA_8888_SkColorType,
			kPremul_SkAlphaType,
			SkColorSpace::MakeSRGB());
		cache.surface = SkSurfaces::RenderTarget(
			GetContext(),
			skgpu::Budgeted::kNo,
			image_info,
			0,
			kTopLeft_GrSurfaceOrigin,
			nullptr);
		cache.width = width;
		cache.height = height;
		return cache.surface;
	}
#endif

	AudioDisplaySkiaOffscreenReadbackCache& AcquireReadbackCache(bool for_present, int width, int height) {
		auto &cache = for_present ? present_readback_cache : content_readback_cache;
		if (cache.width != width || cache.height != height || !cache.bitmap.IsOk()) {
			cache.width = width;
			cache.height = height;
			cache.bitmap = wxBitmap(width, height, 32);
			cache.pixels.resize(static_cast<size_t>(std::max(0, width)) * static_cast<size_t>(std::max(0, height)));
		}
		return cache;
	}

	AudioDisplaySkiaHostBackend GetBackend() const override {
		return AudioDisplaySkiaHostBackend::ExperimentalGpu;
	}

	std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) override {
		// Content backing is written to a wxBitmap for the legacy wxDC
		// repaint path.  Using a GPU offscreen target here would add a
		// pointless CPU→GPU→CPU roundtrip (upload pixels, draw to GPU
		// surface, readback).  Use the bitmap target instead.
		return CreateAudioDisplaySkiaBitmapTarget(bitmap, rect);
	}

	std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) override {
		auto target = std::make_unique<AudioDisplaySkiaGpuOffscreenPresentTarget>(*this, rect);
		if (!target->IsValid())
			return nullptr;
		return target;
	}
};

AudioDisplaySkiaGpuOffscreenContentTarget::AudioDisplaySkiaGpuOffscreenContentTarget(
	AudioDisplaySkiaGpuOffscreenHost &host,
	wxBitmap &bitmap,
	wxRect const& rect)
: host(host)
, rect(rect)
, dest_bitmap(&bitmap)
, readback(nullptr) {
#ifdef WITH_SKIA
	if (!host.EnsureCurrent() || rect.width <= 0 || rect.height <= 0 || !bitmap.IsOk())
		return;

	surface = host.AcquireSurface(false, rect.width, rect.height);
	if (surface)
		readback = &host.AcquireReadbackCache(false, rect.width, rect.height);
#endif
}

bool AudioDisplaySkiaGpuOffscreenContentTarget::IsValid() const {
	return readback
		&& readback->bitmap.IsOk()
#ifdef WITH_SKIA
		&& static_cast<bool>(surface)
#endif
		;
}

SkCanvas *AudioDisplaySkiaGpuOffscreenContentTarget::GetCanvas() const {
#ifdef WITH_SKIA
	return surface ? surface->getCanvas() : nullptr;
#else
	return nullptr;
#endif
}

bool AudioDisplaySkiaGpuOffscreenContentTarget::Finalize() {
#ifdef WITH_SKIA
	if (!IsValid() || !host.EnsureCurrent())
		return false;

	host.Flush();
	auto const image_info = SkImageInfo::Make(
		rect.width,
		rect.height,
		kBGRA_8888_SkColorType,
		kPremul_SkAlphaType,
		SkColorSpace::MakeSRGB());
	if (!surface->readPixels(image_info, readback->pixels.data(), static_cast<size_t>(rect.width) * 4, 0, 0))
		return false;
	if (!CopyBgraPixelsToBitmap(readback->pixels, rect.width, rect.height, readback->bitmap))
		return false;
	if (dest_bitmap && dest_bitmap != &readback->bitmap && dest_bitmap->IsOk()
		&& dest_bitmap->GetWidth() == rect.width && dest_bitmap->GetHeight() == rect.height) {
		return CopyBgraPixelsToBitmap(readback->pixels, rect.width, rect.height, *dest_bitmap);
	}
	return true;
#else
	return false;
#endif
}

AudioDisplaySkiaGpuOffscreenPresentTarget::AudioDisplaySkiaGpuOffscreenPresentTarget(
	AudioDisplaySkiaGpuOffscreenHost &host,
	wxRect const& rect)
: host(host)
, rect(rect)
, readback(nullptr) {
#ifdef WITH_SKIA
	if (!host.EnsureCurrent() || rect.width <= 0 || rect.height <= 0)
		return;

	surface = host.AcquireSurface(true, rect.width, rect.height);
	if (surface)
		readback = &host.AcquireReadbackCache(true, rect.width, rect.height);
#endif
}

bool AudioDisplaySkiaGpuOffscreenPresentTarget::IsValid() const {
	return readback
		&& readback->bitmap.IsOk()
#ifdef WITH_SKIA
		&& static_cast<bool>(surface)
#endif
		;
}

SkCanvas *AudioDisplaySkiaGpuOffscreenPresentTarget::GetCanvas() const {
#ifdef WITH_SKIA
	return surface ? surface->getCanvas() : nullptr;
#else
	return nullptr;
#endif
}

bool AudioDisplaySkiaGpuOffscreenPresentTarget::Finalize() {
#ifdef WITH_SKIA
	if (!IsValid() || !host.EnsureCurrent())
		return false;

	host.Flush();
	auto const image_info = SkImageInfo::Make(
		rect.width,
		rect.height,
		kBGRA_8888_SkColorType,
		kPremul_SkAlphaType,
		SkColorSpace::MakeSRGB());
	if (!surface->readPixels(image_info, readback->pixels.data(), static_cast<size_t>(rect.width) * 4, 0, 0))
		return false;
	return CopyBgraPixelsToBitmap(readback->pixels, rect.width, rect.height, readback->bitmap);
#else
	return false;
#endif
}

bool AudioDisplaySkiaGpuOffscreenPresentTarget::PresentTo(wxDC &dc, bool use_mask) {
	if (!Finalize())
		return false;
	dc.DrawBitmap(readback->bitmap, rect.x, rect.y, use_mask);
	return true;
}

}

// ---------------------------------------------------------------------------
// Direct-GPU present host — renders to AudioDisplay's own wxGLCanvas
// (which is the AudioDisplay itself) and presents via SwapBuffers.
// The GL canvas and context are owned by AudioDisplay, not by this host.
// ---------------------------------------------------------------------------

class AudioDisplaySkiaDirectGpuHost;

class AudioDisplaySkiaDirectGpuPresentTarget final : public AudioDisplaySkiaPresentTarget {
	AudioDisplaySkiaDirectGpuHost &host;
	wxRect rect;
#ifdef WITH_SKIA
	sk_sp<SkSurface> surface;
#endif

public:
	AudioDisplaySkiaDirectGpuPresentTarget(AudioDisplaySkiaDirectGpuHost &host, wxRect const& rect);
	bool IsValid() const override;
	wxRect const& GetRect() const override { return rect; }
	SkCanvas *GetCanvas() const override;
	bool Finalize() override;
	bool PresentTo(wxDC &dc, bool use_mask) override;
};

class AudioDisplaySkiaDirectGpuHost final : public AudioDisplaySkiaHost {
	wxGLCanvas *canvas = nullptr;     // not owned — AudioDisplay IS the canvas
	wxGLContext *gl_ctx = nullptr;    // not owned — AudioDisplay owns it
	std::unique_ptr<SkiaGpuContextHost> context_host;

	// Present surface cache — wraps FBO 0.
	int present_width = 0;
	int present_height = 0;
#ifdef WITH_SKIA
	sk_sp<SkSurface> present_surface;
#endif

public:
	AudioDisplaySkiaDirectGpuHost(wxGLCanvas *canvas, wxGLContext *gl_ctx)
	: canvas(canvas), gl_ctx(gl_ctx) {
	}

	~AudioDisplaySkiaDirectGpuHost() override {
		present_surface.reset();
		if (context_host) context_host->Reset();
		context_host.reset();
		// canvas and gl_ctx are not owned — do not delete
	}

	wxGLCanvas *GetCanvas() const { return canvas; }

	bool EnsureCurrent() {
		if (!canvas || !gl_ctx)
			return false;
		canvas->SetCurrent(*gl_ctx);
		if (!context_host)
			context_host = std::make_unique<SkiaGpuContextHost>();
		return context_host && context_host->EnsureCurrentContext();
	}

#ifdef WITH_SKIA
	GrDirectContext *GetContext() const {
		return context_host ? context_host->Get() : nullptr;
	}

	void Flush() {
		if (context_host) context_host->FlushAndSubmit();
	}

	sk_sp<SkSurface> AcquirePresentSurface(int width, int height) {
		if (!EnsureCurrent() || width <= 0 || height <= 0)
			return nullptr;

		if (present_surface && present_width == width && present_height == height)
			return present_surface;

		// Invalidate stale surface.
		present_surface.reset();

		GrGLFramebufferInfo fb_info;
		fb_info.fFBOID = 0;
		fb_info.fFormat = 0x8058; // GL_RGBA8
		auto backend_rt = GrBackendRenderTargets::MakeGL(width, height, 0, 8, fb_info);
		present_surface = SkSurfaces::WrapBackendRenderTarget(
			GetContext(),
			backend_rt,
			kBottomLeft_GrSurfaceOrigin,
			kRGBA_8888_SkColorType,
			SkColorSpace::MakeSRGB(),
			nullptr);
		present_width = width;
		present_height = height;
		return present_surface;
	}
#endif

	void InvalidatePresentSurface() {
		present_surface.reset();
		present_width = 0;
		present_height = 0;
	}

	AudioDisplaySkiaHostBackend GetBackend() const override {
		return AudioDisplaySkiaHostBackend::DirectGpu;
	}

	std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) override {
		return CreateAudioDisplaySkiaBitmapTarget(bitmap, rect);
	}

	std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) override {
		auto target = std::make_unique<AudioDisplaySkiaDirectGpuPresentTarget>(*this, rect);
		if (!target->IsValid())
			return nullptr;
		return target;
	}
};

AudioDisplaySkiaDirectGpuPresentTarget::AudioDisplaySkiaDirectGpuPresentTarget(
	AudioDisplaySkiaDirectGpuHost &host, wxRect const& rect)
: host(host), rect(rect) {
#ifdef WITH_SKIA
	if (!host.EnsureCurrent() || rect.width <= 0 || rect.height <= 0)
		return;
	surface = host.AcquirePresentSurface(rect.width, rect.height);
#endif
}

bool AudioDisplaySkiaDirectGpuPresentTarget::IsValid() const {
#ifdef WITH_SKIA
	return static_cast<bool>(surface);
#else
	return false;
#endif
}

SkCanvas *AudioDisplaySkiaDirectGpuPresentTarget::GetCanvas() const {
#ifdef WITH_SKIA
	return surface ? surface->getCanvas() : nullptr;
#else
	return nullptr;
#endif
}

bool AudioDisplaySkiaDirectGpuPresentTarget::Finalize() {
#ifdef WITH_SKIA
	if (!IsValid() || !host.EnsureCurrent())
		return false;
	host.Flush();
	return true;
#else
	return false;
#endif
}

bool AudioDisplaySkiaDirectGpuPresentTarget::PresentTo(wxDC & /*dc*/, bool /*use_mask*/) {
	if (!Finalize())
		return false;
	auto *canvas = host.GetCanvas();
	if (!canvas)
		return false;
	canvas->SwapBuffers();
	return true;
}

std::unique_ptr<AudioDisplaySkiaHost> CreateAudioDisplaySkiaBitmapHost() {
	return std::make_unique<AudioDisplaySkiaBitmapHost>();
}

std::unique_ptr<AudioDisplaySkiaHost> CreateAudioDisplaySkiaExperimentalGpuHost(wxWindow *owner) {
	return std::make_unique<AudioDisplaySkiaGpuOffscreenHost>(owner);
}

std::unique_ptr<AudioDisplaySkiaHost> CreateAudioDisplaySkiaDirectGpuHost(wxGLCanvas *canvas, wxGLContext *gl_ctx) {
	return std::make_unique<AudioDisplaySkiaDirectGpuHost>(canvas, gl_ctx);
}
