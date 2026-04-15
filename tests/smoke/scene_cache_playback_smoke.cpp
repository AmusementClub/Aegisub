#include "../../src/legacy_gl_draw.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

namespace {
using WglCreateContextAttribsArbProc = HGLRC (WINAPI *)(HDC, HGLRC, int const*);

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubSceneCachePlaybackSmokeWindow";
	}

	static void EnsureWindowClassRegistered() {
		static bool registered = false;
		if (registered)
			return;

		WNDCLASSA cls = {};
		cls.lpfnWndProc = DefWindowProcA;
		cls.hInstance = GetModuleHandleA(nullptr);
		cls.lpszClassName = WindowClassName();
		cls.style = CS_OWNDC;

		ATOM atom = RegisterClassA(&cls);
		if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			throw std::runtime_error("RegisterClassA failed for scene cache playback smoke window.");

		registered = true;
	}

public:
	HiddenGLWindow(int new_width, int new_height)
	: width(new_width)
	, height(new_height) {
		EnsureWindowClassRegistered();

		hwnd = CreateWindowExA(
			0,
			WindowClassName(),
			"Aegisub scene cache playback smoke",
			WS_POPUP,
			0,
			0,
			width,
			height,
			nullptr,
			nullptr,
			GetModuleHandleA(nullptr),
			nullptr);
		if (!hwnd)
			throw std::runtime_error("CreateWindowExA failed for scene cache playback smoke window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for scene cache playback smoke window.");

		PIXELFORMATDESCRIPTOR pfd = {};
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		pfd.cAlphaBits = 8;
		pfd.cDepthBits = 24;
		pfd.cStencilBits = 8;
		pfd.iLayerType = PFD_MAIN_PLANE;

		int pixel_format = ChoosePixelFormat(dc, &pfd);
		if (!pixel_format)
			throw std::runtime_error("ChoosePixelFormat failed for scene cache playback smoke window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for scene cache playback smoke window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for scene cache playback smoke window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for scene cache playback smoke window.");

		auto create_context_attribs = reinterpret_cast<WglCreateContextAttribsArbProc>(
			wglGetProcAddress("wglCreateContextAttribsARB"));
		if (create_context_attribs) {
			int const attribs[] = {
				WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
				WGL_CONTEXT_MINOR_VERSION_ARB, 3,
				WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
				0
			};
			HGLRC modern_context = create_context_attribs(dc, nullptr, attribs);
			if (modern_context) {
				wglMakeCurrent(nullptr, nullptr);
				wglDeleteContext(legacy_context);
				context = modern_context;
				if (!wglMakeCurrent(dc, context))
					throw std::runtime_error("wglMakeCurrent failed for modern scene cache playback smoke window.");
			}
			else {
				context = legacy_context;
			}
		}
		else {
			context = legacy_context;
		}

		glViewport(0, 0, width, height);
		glDisable(GL_DITHER);
	}

	~HiddenGLWindow() {
		if (wglGetCurrentContext() == context)
			wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (dc && hwnd)
			ReleaseDC(hwnd, dc);
		if (hwnd)
			DestroyWindow(hwnd);
	}

	void MakeCurrent() {
		if (!wglMakeCurrent(dc, context))
			throw std::runtime_error("wglMakeCurrent failed for scene cache playback smoke window.");
	}

	std::vector<unsigned char> ReadBackRgbaTopLeft() {
		MakeCurrent();
		glFinish();
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadBuffer(GL_BACK);

		std::vector<unsigned char> raw(static_cast<std::size_t>(width) * height * 4);
		std::vector<unsigned char> flipped(static_cast<std::size_t>(width) * height * 4);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

		std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
		for (int y = 0; y < height; ++y) {
			auto const* src = raw.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes;
			auto* dst = flipped.data() + static_cast<std::size_t>(y) * row_bytes;
			std::memcpy(dst, src, row_bytes);
		}

		return flipped;
	}
};

template <typename Proc>
Proc LoadOptionalProc(char const *name, char const *fallback_name = nullptr) {
	if (auto *proc = opengl::GetProcAddress(name))
		return reinterpret_cast<Proc>(proc);
	if (fallback_name) {
		if (auto *proc = opengl::GetProcAddress(fallback_name))
			return reinterpret_cast<Proc>(proc);
	}
	return nullptr;
}

struct FramebufferFunctions {
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
};

FramebufferFunctions const& GetFramebufferFunctions() {
	static const FramebufferFunctions functions = {
		LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT"),
		LoadOptionalProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT"),
		LoadOptionalProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT"),
		LoadOptionalProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"),
	};
	return functions;
}

bool PixelMatchesColor(
	std::vector<unsigned char> const& pixels,
	int width,
	int x,
	int y,
	int min_r,
	int min_g,
	int min_b,
	int max_r,
	int max_g,
	int max_b) {
	auto const sample_index = static_cast<std::size_t>(y * width + x) * 4;
	int const sample_r = pixels[sample_index + 0];
	int const sample_g = pixels[sample_index + 1];
	int const sample_b = pixels[sample_index + 2];
	return sample_r >= min_r && sample_r <= max_r
		&& sample_g >= min_g && sample_g <= max_g
		&& sample_b >= min_b && sample_b <= max_b;
}

void ClearRect(int x, int y, int width, int height, float r, float g, float b) {
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, y, width, height);
	glClearColor(r, g, b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
}

void DrawOverlayMarker(int canvas_width, int canvas_height) {
	legacy_gl::ResetCompatibilityState();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glColor4ub(255, 0, 255, 255);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, canvas_width, canvas_height, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	GLfloat const vertices[] = {
		12.0f, 12.0f,
		30.0f, 12.0f,
		30.0f, 30.0f,
		12.0f, 30.0f
	};
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
}

bool ValidateSceneCachePlayback() {
	constexpr int width = 128;
	constexpr int height = 96;

	HiddenGLWindow window(width, height);
	window.MakeCurrent();

	auto const& gl = GetFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		std::cout << "FBO functions unavailable, skipping scene cache playback smoke." << std::endl;
		return true;
	}

	GLuint scene_texture = 0;
	GLuint scene_framebuffer = 0;
	glGenTextures(1, &scene_texture);
	gl.GenFramebuffers(1, &scene_framebuffer);

	glBindTexture(GL_TEXTURE_2D, scene_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, scene_framebuffer);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, scene_texture, 0);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		throw std::runtime_error("scene cache playback smoke could not create a complete framebuffer.");

	glViewport(0, 0, width, height);
	glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ClearRect(0, 0, width / 2, height / 2, 1.0f, 0.0f, 0.0f);
	ClearRect(width / 2, 0, width / 2, height / 2, 0.0f, 1.0f, 0.0f);
	ClearRect(0, height / 2, width / 2, height / 2, 0.0f, 0.0f, 1.0f);
	ClearRect(width / 2, height / 2, width / 2, height / 2, 1.0f, 1.0f, 0.0f);

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
	glViewport(0, 0, width, height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	auto const& compat = legacy_gl::GetCompatibilityFunctions();
	if (compat.BindBuffer) {
		compat.BindBuffer(GL_ARRAY_BUFFER, 1);
		compat.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 2);
	}
	if (compat.ActiveTexture)
		compat.ActiveTexture(GL_TEXTURE1);
	if (compat.ClientActiveTexture)
		compat.ClientActiveTexture(GL_TEXTURE1);
	glColor4f(0.25f, 0.5f, 1.0f, 0.5f);

	legacy_gl::DrawTexturedQuad(scene_texture, width, height);
	if (GLenum err = glGetError()) {
		std::cout << "scene cache playback gl error=" << err << std::endl;
		return false;
	}

	DrawOverlayMarker(width, height);
	if (GLenum err = glGetError()) {
		std::cout << "overlay marker gl error=" << err << std::endl;
		return false;
	}

	auto const pixels = window.ReadBackRgbaTopLeft();

	bool const top_left_ok = PixelMatchesColor(pixels, width, width / 4, height / 4, 0, 0, 200, 80, 80, 255);
	bool const top_right_ok = PixelMatchesColor(pixels, width, width * 3 / 4, height / 4, 200, 200, 0, 255, 255, 96);
	bool const bottom_left_ok = PixelMatchesColor(pixels, width, width / 4, height * 3 / 4, 200, 0, 0, 255, 80, 80);
	bool const bottom_right_ok = PixelMatchesColor(pixels, width, width * 3 / 4, height * 3 / 4, 0, 200, 0, 80, 255, 80);
	bool const marker_ok = PixelMatchesColor(pixels, width, 20, 20, 200, 0, 200, 255, 80, 255);

	std::cout
		<< "top_left=" << top_left_ok
		<< " top_right=" << top_right_ok
		<< " bottom_left=" << bottom_left_ok
		<< " bottom_right=" << bottom_right_ok
		<< " marker=" << marker_ok
		<< std::endl;

	gl.DeleteFramebuffers(1, &scene_framebuffer);
	glDeleteTextures(1, &scene_texture);
	return top_left_ok && top_right_ok && bottom_left_ok && bottom_right_ok && marker_ok;
}
}

int main() try {
	bool const passed = ValidateSceneCachePlayback();
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "scene-cache-playback-smoke failed: " << err.what() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "scene-cache-playback-smoke failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "scene-cache-playback-smoke is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
