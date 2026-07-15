#include "../../src/skia/skia_video_compositor.h"
#include "../../src/skia/skia_video_overlay_bounds.h"
#include "../../src/skia/skia_video_overlay_gl.h"
#include "../../src/skia_runtime/skia_surface_provider.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
template <typename Proc>
Proc LoadGlProc(char const *name, char const *fallback_name = nullptr) {
	auto load = [](char const *proc_name) -> void * {
		auto proc = reinterpret_cast<void *>(wglGetProcAddress(proc_name));
		if (proc && proc != reinterpret_cast<void *>(1)
			&& proc != reinterpret_cast<void *>(2)
			&& proc != reinterpret_cast<void *>(3)
			&& proc != reinterpret_cast<void *>(-1)) {
			return proc;
		}
		auto module = GetModuleHandleA("opengl32.dll");
		return module ? reinterpret_cast<void *>(GetProcAddress(module, proc_name)) : nullptr;
	};
	if (auto proc = load(name))
		return reinterpret_cast<Proc>(proc);
	return fallback_name ? reinterpret_cast<Proc>(load(fallback_name)) : nullptr;
}

struct FramebufferFunctions {
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = LoadGlProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT");
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = LoadGlProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = LoadGlProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT");
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = LoadGlProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT");
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = LoadGlProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = LoadGlProc<PFNGLBINDRENDERBUFFERPROC>("glBindRenderbuffer", "glBindRenderbufferEXT");
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = LoadGlProc<PFNGLDELETERENDERBUFFERSPROC>("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = LoadGlProc<PFNGLGENRENDERBUFFERSPROC>("glGenRenderbuffers", "glGenRenderbuffersEXT");
	PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage = LoadGlProc<PFNGLRENDERBUFFERSTORAGEPROC>("glRenderbufferStorage", "glRenderbufferStorageEXT");
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = LoadGlProc<PFNGLFRAMEBUFFERRENDERBUFFERPROC>("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");

	bool Complete() const noexcept {
		return BindFramebuffer
			&& DeleteFramebuffers
			&& GenFramebuffers
			&& FramebufferTexture2D
			&& CheckFramebufferStatus
			&& BindRenderbuffer
			&& DeleteRenderbuffers
			&& GenRenderbuffers
			&& RenderbufferStorage
			&& FramebufferRenderbuffer;
	}
};

class OffscreenTarget final {
	FramebufferFunctions const& gl;

public:
	GLuint framebuffer = 0;
	GLuint texture = 0;
	GLuint depth_stencil = 0;
	int width = 0;
	int height = 0;

	OffscreenTarget(FramebufferFunctions const& gl, int width, int height)
	: gl(gl)
	, width(width)
	, height(height) {
		if (!gl.Complete() || width <= 0 || height <= 0)
			throw std::runtime_error("bounded overlay FBO entry points are unavailable");

		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glBindTexture(GL_TEXTURE_2D, 0);

		gl.GenFramebuffers(1, &framebuffer);
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
		gl.FramebufferTexture2D(
			GL_FRAMEBUFFER_EXT,
			GL_COLOR_ATTACHMENT0_EXT,
			GL_TEXTURE_2D,
			texture,
			0);

		gl.GenRenderbuffers(1, &depth_stencil);
		gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, depth_stencil);
		gl.RenderbufferStorage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, width, height);
		gl.FramebufferRenderbuffer(
			GL_FRAMEBUFFER_EXT,
			GL_DEPTH_ATTACHMENT_EXT,
			GL_RENDERBUFFER_EXT,
			depth_stencil);
		gl.FramebufferRenderbuffer(
			GL_FRAMEBUFFER_EXT,
			GL_STENCIL_ATTACHMENT_EXT,
			GL_RENDERBUFFER_EXT,
			depth_stencil);
		gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, 0);

		auto const status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
		if (status != GL_FRAMEBUFFER_COMPLETE && status != GL_FRAMEBUFFER_COMPLETE_EXT)
			throw std::runtime_error("bounded overlay FBO is incomplete");
	}

	~OffscreenTarget() {
		if (depth_stencil)
			gl.DeleteRenderbuffers(1, &depth_stencil);
		if (framebuffer)
			gl.DeleteFramebuffers(1, &framebuffer);
		if (texture)
			glDeleteTextures(1, &texture);
	}
};

class HiddenGlWindow final {
	static char const *ClassName() noexcept { return "AegisubSkiaVideoCompositorSmoke"; }

	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

public:
	HiddenGlWindow(int width, int height)
	: width(width)
	, height(height) {
		WNDCLASSA cls = {};
		cls.style = CS_OWNDC;
		cls.lpfnWndProc = DefWindowProcA;
		cls.hInstance = GetModuleHandleA(nullptr);
		cls.lpszClassName = ClassName();
		if (!RegisterClassA(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			throw std::runtime_error("RegisterClassA failed");

		window = CreateWindowExA(
			0,
			ClassName(),
			"Aegisub Skia compositor smoke",
			WS_POPUP,
			0,
			0,
			width,
			height,
			nullptr,
			nullptr,
			GetModuleHandleA(nullptr),
			nullptr);
		if (!window)
			throw std::runtime_error("CreateWindowExA failed");

		dc = GetDC(window);
		if (!dc)
			throw std::runtime_error("GetDC failed");

		PIXELFORMATDESCRIPTOR descriptor = {};
		descriptor.nSize = sizeof(descriptor);
		descriptor.nVersion = 1;
		descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		descriptor.iPixelType = PFD_TYPE_RGBA;
		descriptor.cColorBits = 32;
		descriptor.cAlphaBits = 8;
		descriptor.cDepthBits = 24;
		descriptor.cStencilBits = 8;
		descriptor.iLayerType = PFD_MAIN_PLANE;
		int const pixel_format = ChoosePixelFormat(dc, &descriptor);
		if (!pixel_format || !SetPixelFormat(dc, pixel_format, &descriptor))
			throw std::runtime_error("OpenGL pixel format setup failed");

		context = wglCreateContext(dc);
		if (!context || !wglMakeCurrent(dc, context))
			throw std::runtime_error("WGL context setup failed");
		glViewport(0, 0, width, height);
	}

	~HiddenGlWindow() {
		if (wglGetCurrentContext() == context)
			wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (dc && window)
			ReleaseDC(window, dc);
		if (window)
			DestroyWindow(window);
	}

	SkiaGlContextToken Token(std::uint64_t generation) const noexcept {
		return { context, generation };
	}

	int Width() const noexcept { return width; }
	int Height() const noexcept { return height; }

	SkiaVideoFrameTarget Target(std::uint64_t context_generation, std::uint64_t present_generation) const {
		GLint sample_count = 0;
		GLint stencil_bits = 0;
#ifdef GL_SAMPLES
		glGetIntegerv(GL_SAMPLES, &sample_count);
#endif
		glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);

		SkiaVideoFrameTarget target;
		target.framebuffer_id = 0;
		target.context_generation = context_generation;
		target.width = width;
		target.height = height;
		target.viewport = { 0, 0, width, height };
		target.origin = SkiaVideoTargetOrigin::BottomLeft;
		target.sample_count = std::max(0, sample_count);
		target.stencil_bits = std::max(0, stencil_bits);
		target.pixel_format = SkiaVideoTargetPixelFormat::Rgba8;
		target.color_space = SkiaVideoTargetColorSpace::SdrPreview;
		target.hdr_to_sdr_complete = true;
		target.present_generation = present_generation;
		return target;
	}

	std::vector<unsigned char> ReadBack() const {
		glFinish();
		glReadBuffer(GL_BACK);
		std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		return pixels;
	}
};

void ClearBackBuffer(HiddenGlWindow const& window) {
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	glViewport(0, 0, window.Width(), window.Height());
	glDisable(GL_DITHER);
	glClearColor(0.125f, 0.25f, 0.375f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

struct OverlayRenderResult {
	std::vector<unsigned char> pixels;
	std::vector<unsigned char> cached_pixels;
	bool back_buffer_unchanged_before_composite = false;
};

OverlayRenderResult RenderOverlayTarget(
	HiddenGlWindow& window,
	FramebufferFunctions const& gl,
	SkiaVideoCompositor& compositor,
	SkiaGlContextToken token,
	SkiaOverlayDeviceBounds const& normal_bounds,
	SkiaOverlayDeviceBounds const& invert_bounds,
	float device_scale,
	std::uint64_t present_generation) {
	ClearBackBuffer(window);
	auto const untouched = window.ReadBack();

	OffscreenTarget normal(gl, normal_bounds.width, normal_bounds.height);
	OffscreenTarget invert(gl, invert_bounds.width, invert_bounds.height);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	if (!compositor.BeginFrame(token, window.Target(token.generation, present_generation)))
		throw std::runtime_error("bounded overlay BeginFrame failed");

	SkiaSurfaceProvider surface_provider;
	SkiaFramebufferSurfaceDescriptor descriptor;
	descriptor.width = normal_bounds.width;
	descriptor.height = normal_bounds.height;
	descriptor.stencil_bits = 8;
	descriptor.bottom_left_origin = false;
	descriptor.framebuffer_id = normal.framebuffer;
	auto normal_surface = surface_provider.AcquireFramebufferSurface(compositor.Device().Get(), descriptor);
	descriptor.width = invert_bounds.width;
	descriptor.height = invert_bounds.height;
	descriptor.framebuffer_id = invert.framebuffer;
	auto invert_surface = surface_provider.AcquireFramebufferSurface(compositor.Device().Get(), descriptor);
	if (!normal_surface || !invert_surface)
		throw std::runtime_error("bounded overlay Skia surface acquisition failed");

	auto *normal_canvas = normal_surface->getCanvas();
	auto *invert_canvas = invert_surface->getCanvas();
	if (!normal_canvas || !invert_canvas)
		throw std::runtime_error("bounded overlay surface has no canvas");
	normal_canvas->clear(SK_ColorTRANSPARENT);
	invert_canvas->clear(SK_ColorTRANSPARENT);
	normal_canvas->save();
	invert_canvas->save();
	normal_canvas->scale(device_scale, device_scale);
	invert_canvas->scale(device_scale, device_scale);
	normal_canvas->translate(
		-static_cast<float>(normal_bounds.x) / device_scale,
		-static_cast<float>(normal_bounds.y) / device_scale);
	invert_canvas->translate(
		-static_cast<float>(invert_bounds.x) / device_scale,
		-static_cast<float>(invert_bounds.y) / device_scale);

	SkPaint normal_paint;
	normal_paint.setAntiAlias(true);
	normal_paint.setColor(SkColorSetARGB(160, 240, 50, 30));
	normal_canvas->drawRect(
		SkRect::MakeLTRB(
			42.0f / device_scale,
			24.0f / device_scale,
			56.0f / device_scale,
			38.0f / device_scale),
		normal_paint);
	SkPaint invert_paint;
	invert_paint.setAntiAlias(false);
	invert_paint.setColor(SK_ColorWHITE);
	invert_canvas->drawRect(
		SkRect::MakeLTRB(
			47.0f / device_scale,
			28.0f / device_scale,
			51.0f / device_scale,
			35.0f / device_scale),
		invert_paint);
	normal_canvas->restore();
	invert_canvas->restore();
	if (!compositor.FinishFrame(token, true))
		throw std::runtime_error("bounded overlay FinishFrame failed");

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	OverlayRenderResult result;
	result.back_buffer_unchanged_before_composite = untouched == window.ReadBack();
	glViewport(0, 0, window.Width(), window.Height());
	CompositeSkiaVideoOverlayTextures(
		normal.texture,
		true,
		normal_bounds,
		invert.texture,
		true,
		invert_bounds,
		window.Width(),
		window.Height());
	result.pixels = window.ReadBack();

	ClearBackBuffer(window);
	CompositeSkiaVideoOverlayTextures(
		normal.texture,
		true,
		normal_bounds,
		invert.texture,
		true,
		invert_bounds,
		window.Width(),
		window.Height());
	result.cached_pixels = window.ReadBack();
	return result;
}

bool ValidateBoundedOverlayParity(HiddenGlWindow& window) {
	FramebufferFunctions const gl;
	if (!gl.Complete())
		throw std::runtime_error("framebuffer functions are unavailable");

	auto const token = window.Token(5);
	SkiaVideoCompositor compositor(SkiaVideoFailureInjection::None);
	auto const full = RenderOverlayTarget(
		window,
		gl,
		compositor,
		token,
		{ 0, 0, window.Width(), window.Height() },
		{ 0, 0, window.Width(), window.Height() },
		2.0f,
		1);
	auto const bounded = RenderOverlayTarget(
		window,
		gl,
		compositor,
		token,
		{ 32, 16, 32, 32 },
		{ 32, 0, 32, 64 },
		2.0f,
		2);
	bool const passed =
		full.back_buffer_unchanged_before_composite
		&& bounded.back_buffer_unchanged_before_composite
		&& full.pixels == full.cached_pixels
		&& bounded.pixels == bounded.cached_pixels
		&& full.pixels == bounded.pixels;
	std::cout
		<< "bounded-overlay unchanged="
		<< (bounded.back_buffer_unchanged_before_composite ? "yes" : "no")
		<< " cached=" << (bounded.pixels == bounded.cached_pixels ? "exact" : "different")
		<< " full-parity=" << (full.pixels == bounded.pixels ? "exact" : "different")
		<< " dpi-scale=2"
		<< '\n';
	compositor.Release(token);
	return passed;
}

bool ValidateNonDrawingProbe(HiddenGlWindow& window) {
	glDrawBuffer(GL_BACK);
	glClearColor(0.125f, 0.375f, 0.625f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	auto const before = window.ReadBack();

	auto const token = window.Token(1);
	SkiaVideoCompositor compositor(SkiaVideoFailureInjection::None);
	bool const probed = compositor.ProbeFrame(token, window.Target(1, 1));
	bool const empty_submit = compositor.BeginFrame(token, window.Target(1, 2))
		&& compositor.FinishFrame(token, true);
	auto const after = window.ReadBack();

	std::cout
		<< "probe=" << (probed ? "pass" : "fail")
		<< " empty_submit=" << (empty_submit ? "pass" : "fail")
		<< " unchanged=" << (before == after ? "yes" : "no")
		<< " vendor=" << compositor.Device().GlVendor()
		<< " renderer=" << compositor.Device().GlRenderer()
		<< " version=" << compositor.Device().GlVersion()
		<< '\n';
	bool const passed = probed
		&& empty_submit
		&& compositor.Device().Health() == SkiaGlDeviceHealth::Healthy
		&& before == after;
	compositor.Release(token);
	return passed;
}

bool ValidateStickyInjection(
	HiddenGlWindow& window,
	SkiaVideoFailureInjection injection,
	SkiaGlDeviceFailure expected_failure,
	std::uint64_t generation) {
	auto const token = window.Token(generation);
	SkiaVideoCompositor compositor(injection);
	bool const first = compositor.ProbeFrame(token, window.Target(generation, 1));
	bool const second = compositor.ProbeFrame(token, window.Target(generation, 2));
	bool const passed = !first
		&& !second
		&& compositor.Device().Health() == SkiaGlDeviceHealth::Unhealthy
		&& compositor.Device().LastFailure() == expected_failure;
	std::cout
		<< "injection=" << ToString(injection)
		<< " sticky=" << (passed ? "yes" : "no")
		<< " failure=" << ToString(compositor.Device().LastFailure())
		<< '\n';
	compositor.Release(token);
	return passed;
}
}

int main() try {
	HiddenGlWindow window(96, 72);
	bool passed = ValidateNonDrawingProbe(window);
	passed = ValidateStickyInjection(
		window,
		SkiaVideoFailureInjection::ContextInitialization,
		SkiaGlDeviceFailure::ContextInitializationInjected,
		2) && passed;
	passed = ValidateStickyInjection(
		window,
		SkiaVideoFailureInjection::FlushSubmit,
		SkiaGlDeviceFailure::FlushInjected,
		3) && passed;
	passed = ValidateStickyInjection(
		window,
		SkiaVideoFailureInjection::FrameBegin,
		SkiaGlDeviceFailure::FrameBeginInjected,
		4) && passed;
	passed = ValidateBoundedOverlayParity(window) && passed;
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "skia-video-compositor-smoke failed: " << err.what() << '\n';
	return 2;
}

#else

#include <iostream>

int main() {
	std::cout << "skia-video-compositor-smoke is currently only implemented on Windows builds.\n";
	return 0;
}

#endif
