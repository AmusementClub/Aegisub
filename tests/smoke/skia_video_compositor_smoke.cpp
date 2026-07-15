#include "../../src/skia/skia_video_compositor.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GL/gl.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
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
	HiddenGlWindow window(64, 48);
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
