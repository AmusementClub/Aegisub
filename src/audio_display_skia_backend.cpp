// Copyright (c) 2026
// All rights reserved.

#include "audio_display_skia_backend.h"

#include "audio_display_skia_target.h"
#include "skia_runtime/skia_gpu_context_host.h"

#ifdef WITH_SKIA
#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <include/core/SkColorSpace.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLTypes.h>
#endif

#include <wx/glcanvas.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace {
#if wxCHECK_VERSION(3, 1, 1)
int audio_display_gl_attribs[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, WX_GL_BUFFER_SIZE, 24, WX_GL_MIN_ALPHA, 8, 0 };
#else
int audio_display_gl_attribs[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

#ifdef WITH_SKIA
std::string ReadGlString(GLenum name) {
	auto const* value = reinterpret_cast<char const*>(glGetString(name));
	return value ? std::string(value) : std::string();
}
#endif

std::string ToLowerAscii(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

bool ContainsCaseInsensitive(std::string const& haystack, char const* needle) {
	if (!needle || !*needle)
		return false;
	return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

bool LooksSoftwareLike(AudioDisplaySkiaGpuDiagnostics const& info) {
	auto const combined = info.vendor + " " + info.renderer + " " + info.version;
	return ContainsCaseInsensitive(combined, "gdi generic")
		|| ContainsCaseInsensitive(combined, "software rasterizer")
		|| ContainsCaseInsensitive(combined, "llvmpipe")
		|| ContainsCaseInsensitive(combined, "softpipe")
		|| ContainsCaseInsensitive(combined, "swiftshader")
		|| ContainsCaseInsensitive(combined, "basic render")
		|| ContainsCaseInsensitive(combined, "microsoft basic render");
}

bool PopulateGpuDiagnostics(AudioDisplaySkiaGpuDiagnostics &out) {
	out = AudioDisplaySkiaGpuDiagnostics();
#ifdef WITH_SKIA
	out.vendor = ReadGlString(GL_VENDOR);
	out.renderer = ReadGlString(GL_RENDERER);
	out.version = ReadGlString(GL_VERSION);
#ifdef GL_SHADING_LANGUAGE_VERSION
	out.shading_language_version = ReadGlString(GL_SHADING_LANGUAGE_VERSION);
#endif
	out.available = !out.vendor.empty() || !out.renderer.empty() || !out.version.empty();
	out.software_like = out.available && LooksSoftwareLike(out);
#endif
	return out.available;
}

class AudioDisplaySkiaBitmapBackend final : public AudioDisplaySkiaBackend {
public:
	AudioDisplaySkiaBackendType GetBackendType() const override {
		return AudioDisplaySkiaBackendType::Bitmap;
	}

	std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) override {
		return CreateAudioDisplaySkiaBitmapTarget(bitmap, rect);
	}

	std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) override {
		return CreateAudioDisplaySkiaBitmapPresentTarget(rect);
	}

	bool QueryGpuDiagnostics(AudioDisplaySkiaGpuDiagnostics &out) override {
		out = AudioDisplaySkiaGpuDiagnostics();
		return false;
	}

#ifdef WITH_SKIA
	sk_sp<SkSurface> AcquireCachedContentSurface(int width, int height) override {
		(void)width;
		(void)height;
		return nullptr;
	}
#endif
};

class AudioDisplaySkiaGpuBackend;

class AudioDisplaySkiaGpuPresentTarget final : public AudioDisplaySkiaPresentTarget {
	AudioDisplaySkiaGpuBackend &host;
	wxRect rect;
#ifdef WITH_SKIA
	sk_sp<SkSurface> surface;
#endif

public:
	AudioDisplaySkiaGpuPresentTarget(AudioDisplaySkiaGpuBackend &host, wxRect const& rect);
	bool IsValid() const override;
	wxRect const& GetRect() const override { return rect; }
	SkCanvas *GetCanvas() const override;
	bool Finalize() override;
	bool PresentTo(wxDC &dc, bool use_mask) override;
};

class AudioDisplaySkiaGpuBackend final : public AudioDisplaySkiaBackend {
	wxGLCanvas *canvas = nullptr;
	wxGLContext *gl_ctx = nullptr;
	std::unique_ptr<SkiaGpuContextHost> context_host;
	int present_width = 0;
	int present_height = 0;
#ifdef WITH_SKIA
	sk_sp<SkSurface> present_surface;
	sk_sp<SkSurface> content_surface;
	int content_width = 0;
	int content_height = 0;
#endif

public:
	AudioDisplaySkiaGpuBackend(wxGLCanvas *canvas, wxGLContext *gl_ctx)
	: canvas(canvas), gl_ctx(gl_ctx) {
	}

	~AudioDisplaySkiaGpuBackend() override {
#ifdef WITH_SKIA
		content_surface.reset();
		present_surface.reset();
#endif
		if (context_host)
			context_host->Reset();
		context_host.reset();
	}

	wxGLCanvas *GetCanvas() const {
		return canvas;
	}

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
		if (context_host)
			context_host->FlushAndSubmit();
	}

	sk_sp<SkSurface> AcquireCachedContentSurfaceInternal(int width, int height) {
		if (!EnsureCurrent() || width <= 0 || height <= 0)
			return nullptr;

		if (content_surface && content_width == width && content_height == height)
			return content_surface;

		content_surface.reset();
		content_surface = SkSurfaces::RenderTarget(
			GetContext(),
			skgpu::Budgeted::kNo,
			SkImageInfo::Make(
				width,
				height,
				kBGRA_8888_SkColorType,
				kPremul_SkAlphaType,
				SkColorSpace::MakeSRGB()),
			0,
			kTopLeft_GrSurfaceOrigin,
			nullptr,
			false);
		content_width = width;
		content_height = height;
		return content_surface;
	}

	sk_sp<SkSurface> AcquirePresentSurface(int width, int height) {
		if (!EnsureCurrent() || width <= 0 || height <= 0)
			return nullptr;

		if (present_surface && present_width == width && present_height == height)
			return present_surface;

		present_surface.reset();

		GrGLFramebufferInfo fb_info;
		fb_info.fFBOID = 0;
		fb_info.fFormat = 0x8058;
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

	AudioDisplaySkiaBackendType GetBackendType() const override {
		return AudioDisplaySkiaBackendType::Gpu;
	}

	std::unique_ptr<AudioDisplaySkiaTarget> CreateContentTarget(wxBitmap &bitmap, wxRect const& rect) override {
		return CreateAudioDisplaySkiaBitmapTarget(bitmap, rect);
	}

	std::unique_ptr<AudioDisplaySkiaPresentTarget> CreatePresentTarget(wxRect const& rect) override {
		auto target = std::make_unique<AudioDisplaySkiaGpuPresentTarget>(*this, rect);
		if (!target->IsValid())
			return nullptr;
		return target;
	}

	bool QueryGpuDiagnostics(AudioDisplaySkiaGpuDiagnostics &out) override {
		if (!EnsureCurrent()) {
			out = AudioDisplaySkiaGpuDiagnostics();
			return false;
		}
		return PopulateGpuDiagnostics(out);
	}

#ifdef WITH_SKIA
	sk_sp<SkSurface> AcquireCachedContentSurface(int width, int height) override {
		return AcquireCachedContentSurfaceInternal(width, height);
	}
#endif
};

AudioDisplaySkiaGpuPresentTarget::AudioDisplaySkiaGpuPresentTarget(
	AudioDisplaySkiaGpuBackend &host,
	wxRect const& rect)
: host(host)
, rect(rect) {
#ifdef WITH_SKIA
	if (!host.EnsureCurrent() || rect.width <= 0 || rect.height <= 0)
		return;
	surface = host.AcquirePresentSurface(rect.width, rect.height);
#endif
}

bool AudioDisplaySkiaGpuPresentTarget::IsValid() const {
#ifdef WITH_SKIA
	return static_cast<bool>(surface);
#else
	return false;
#endif
}

SkCanvas *AudioDisplaySkiaGpuPresentTarget::GetCanvas() const {
#ifdef WITH_SKIA
	return surface ? surface->getCanvas() : nullptr;
#else
	return nullptr;
#endif
}

bool AudioDisplaySkiaGpuPresentTarget::Finalize() {
#ifdef WITH_SKIA
	if (!IsValid() || !host.EnsureCurrent())
		return false;
	host.Flush();
	return true;
#else
	return false;
#endif
}

bool AudioDisplaySkiaGpuPresentTarget::PresentTo(wxDC & /*dc*/, bool /*use_mask*/) {
	if (!Finalize())
		return false;
	auto *canvas = host.GetCanvas();
	if (!canvas)
		return false;
	canvas->SwapBuffers();
	return true;
}
}

int *GetAudioDisplayGlAttribs() {
	return audio_display_gl_attribs;
}

std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaBitmapBackend() {
	return std::make_unique<AudioDisplaySkiaBitmapBackend>();
}

std::unique_ptr<AudioDisplaySkiaBackend> CreateAudioDisplaySkiaGpuBackend(wxGLCanvas *canvas, wxGLContext *gl_ctx) {
	return std::make_unique<AudioDisplaySkiaGpuBackend>(canvas, gl_ctx);
}