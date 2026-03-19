#include "../../src/source_frame.h"
#include "../../src/subtitle_overlay.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_renderer_error.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <eh.h>
#ifdef GetMessage
#undef GetMessage
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "../../src/video_renderer_opengl.h"

#include <libaegisub/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
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

void SehTranslator(unsigned int code, EXCEPTION_POINTERS*) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "SEH exception 0x%08X", code);
	throw std::runtime_error(buffer);
}

struct Rgba8 {
	unsigned char r = 0;
	unsigned char g = 0;
	unsigned char b = 0;
	unsigned char a = 255;
};

struct ValidationResult {
	std::string name;
	int max_abs_error = 0;
	double mean_abs_error = 0.0;
	int compared_pixels = 0;
	int max_x = 0;
	int max_y = 0;
	int max_channel = 0;
	std::array<int, 4> reference_rgba = { { 0, 0, 0, 0 } };
	std::array<int, 4> candidate_rgba = { { 0, 0, 0, 0 } };
};

struct ActiveBounds {
	bool valid = false;
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	int non_zero_alpha_pixels = 0;
};

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubOpenGLOverlaySmokeWindow";
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
			throw std::runtime_error("RegisterClassA failed for hidden OpenGL smoke window.");

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
			"Aegisub OpenGL overlay smoke",
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
			throw std::runtime_error("CreateWindowExA failed for hidden OpenGL smoke window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for hidden OpenGL smoke window.");

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
			throw std::runtime_error("ChoosePixelFormat failed for hidden OpenGL smoke window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for hidden OpenGL smoke window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for hidden OpenGL smoke window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for hidden OpenGL smoke window.");

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
					throw std::runtime_error("wglMakeCurrent failed for modern hidden OpenGL smoke window.");
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
			throw std::runtime_error("wglMakeCurrent failed for hidden OpenGL smoke window.");
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

struct NativeFrameStorage {
	std::vector<unsigned char> plane0;
	std::vector<unsigned char> plane1;
};

ValidationResult CompareRgbaImages(
	std::string name,
	std::vector<unsigned char> const& reference,
	std::vector<unsigned char> const& candidate,
	int width,
	int height) {
	ValidationResult result;
	result.name = std::move(name);

	if (reference.size() != candidate.size()) {
		result.max_abs_error = 255;
		return result;
	}

	double total_abs = 0.0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				int ref = reference[base + channel];
				int got = candidate[base + channel];
				int abs_err = std::abs(ref - got);
				total_abs += abs_err;
				++result.compared_pixels;
				if (abs_err > result.max_abs_error) {
					result.max_abs_error = abs_err;
					result.max_x = x;
					result.max_y = y;
					result.max_channel = channel;
					for (int i = 0; i < 4; ++i) {
						result.reference_rgba[static_cast<std::size_t>(i)] = reference[base + i];
						result.candidate_rgba[static_cast<std::size_t>(i)] = candidate[base + i];
					}
				}
			}
		}
	}

	result.mean_abs_error = total_abs / static_cast<double>(reference.size());
	return result;
}

ActiveBounds FindActiveBounds(
	std::vector<unsigned char> const& pixels,
	int width,
	int height) {
	ActiveBounds bounds;
	bounds.x0 = width;
	bounds.y0 = height;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			if (pixels[base + 3] == 0)
				continue;

			bounds.valid = true;
			bounds.non_zero_alpha_pixels++;
			bounds.x0 = std::min(bounds.x0, x);
			bounds.y0 = std::min(bounds.y0, y);
			bounds.x1 = std::max(bounds.x1, x + 1);
			bounds.y1 = std::max(bounds.y1, y + 1);
		}
	}

	if (!bounds.valid) {
		bounds.x0 = 0;
		bounds.y0 = 0;
	}
	return bounds;
}

bool IsStablePixel(
	std::vector<unsigned char> const& pixels,
	int width,
	int height,
	int x,
	int y) {
	if (x <= 0 || y <= 0 || x >= width - 1 || y >= height - 1)
		return false;

	auto same_pixel = [&](int other_x, int other_y) {
		std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
		std::size_t other = (static_cast<std::size_t>(other_y) * width + other_x) * 4;
		return pixels[base + 0] == pixels[other + 0]
			&& pixels[base + 1] == pixels[other + 1]
			&& pixels[base + 2] == pixels[other + 2]
			&& pixels[base + 3] == pixels[other + 3];
	};

	return same_pixel(x - 1, y)
		&& same_pixel(x + 1, y)
		&& same_pixel(x, y - 1)
		&& same_pixel(x, y + 1);
}

ValidationResult CompareRgbaImagesOnStableMask(
	std::string name,
	std::vector<unsigned char> const& mask_source,
	std::vector<unsigned char> const& reference,
	std::vector<unsigned char> const& candidate,
	int width,
	int height) {
	ValidationResult result;
	result.name = std::move(name);

	if (mask_source.size() != reference.size() || reference.size() != candidate.size()) {
		result.max_abs_error = 255;
		return result;
	}

	double total_abs = 0.0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (!IsStablePixel(mask_source, width, height, x, y))
				continue;

			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				int ref = reference[base + channel];
				int got = candidate[base + channel];
				int abs_err = std::abs(ref - got);
				total_abs += abs_err;
				++result.compared_pixels;
				if (abs_err > result.max_abs_error) {
					result.max_abs_error = abs_err;
					result.max_x = x;
					result.max_y = y;
					result.max_channel = channel;
					for (int i = 0; i < 4; ++i) {
						result.reference_rgba[static_cast<std::size_t>(i)] = reference[base + i];
						result.candidate_rgba[static_cast<std::size_t>(i)] = candidate[base + i];
					}
				}
			}
		}
	}

	if (result.compared_pixels > 0)
		result.mean_abs_error = total_abs / static_cast<double>(result.compared_pixels);
	else
		result.max_abs_error = 255;
	return result;
}

void PrintValidationResult(ValidationResult const& result) {
	std::cout
		<< result.name
		<< " compared=" << result.compared_pixels
		<< " max_abs=" << result.max_abs_error
		<< " mean_abs=" << result.mean_abs_error
		<< " worst=(" << result.max_x << "," << result.max_y << "," << result.max_channel << ")"
		<< " ref=("
		<< result.reference_rgba[0] << ","
		<< result.reference_rgba[1] << ","
		<< result.reference_rgba[2] << ","
		<< result.reference_rgba[3] << ")"
		<< " got=("
		<< result.candidate_rgba[0] << ","
		<< result.candidate_rgba[1] << ","
		<< result.candidate_rgba[2] << ","
		<< result.candidate_rgba[3] << ")"
		<< "\n";
}

SubtitleOverlayStorage MakeSolidOverlayStorage(int width, int height, Rgba8 color) {
	SubtitleOverlayStorage storage;
	storage.Reset(width, height, false);
	storage.has_visible_content = true;
	for (int y = 0; y < height; ++y) {
		auto* row = storage.pixels.data() + static_cast<std::size_t>(y) * storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = color.b;
			pixel[1] = color.g;
			pixel[2] = color.r;
			pixel[3] = color.a;
		}
	}
	return storage;
}

SourceFrame MakeNativeNv12SourceFrame(
	int width,
	int height,
	SourceFrameGeometry geometry,
	NativeFrameStorage& storage) {
	auto format = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
	int chroma_width = GetSourceFramePlaneWidth(format, width, 1);
	int chroma_height = GetSourceFramePlaneHeight(format, height, 1);
	storage.plane0.assign(static_cast<std::size_t>(width) * height, 64);
	storage.plane1.assign(static_cast<std::size_t>(chroma_width) * chroma_height * 2, 128);

	SourceFrame frame;
	frame.output_mode = SourceFrameOutputMode::Native;
	frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 23 };
	frame.format_info = format;
	frame.width = width;
	frame.height = height;
	frame.plane_count = format.plane_count;
	frame.geometry = geometry;
	frame.color = SourceFrameColorMetadataFromLegacyColorSpace("TV.709");
	frame.planes[0] = { storage.plane0.data(), width, width, height };
	frame.planes[1] = {
		storage.plane1.data(),
		chroma_width * format.planes[1].bytes_per_sample,
		chroma_width,
		chroma_height
	};
	return frame;
}

std::vector<unsigned char> RenderSecondaryOverlayScenario(SubtitleOverlay const& overlay) {
	HiddenGLWindow window(6, 8);
	OpenGLVideoRenderer renderer(false, true, true);
	window.MakeCurrent();

	NativeFrameStorage native_storage;
	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(12, 10);
	geometry.visible_rect = { 2, 1, 8, 6 };
	geometry.rotation = 90;
	geometry.display_vflip = true;
	auto source = MakeNativeNv12SourceFrame(12, 10, geometry, native_storage);

	renderer.UploadFrame(source);
	renderer.UploadOverlay(&overlay);
	renderer.Render({ 0, 0, 6, 8 }, 6, 8);
	auto result = window.ReadBackRgbaTopLeft();
	window.MakeCurrent();
	renderer.Reset();
	return result;
}

bool RunSecondaryOverlayTransformValidation() {
	std::cout << "Validation for OpenGL secondary overlay transform\n";

	auto storage_overlay_storage = MakeSolidOverlayStorage(9, 5, { 32, 255, 32, 255 });
	auto storage_overlay = storage_overlay_storage.MakeView(true);
	storage_overlay.canvas_width = 12;
	storage_overlay.canvas_height = 10;
	storage_overlay.target_x = 1;
	storage_overlay.target_y = 2;
	storage_overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceStorage;
	storage_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto visible_overlay_storage = MakeSolidOverlayStorage(8, 5, { 32, 255, 32, 255 });
	auto visible_overlay = visible_overlay_storage.MakeView(true);
	visible_overlay.canvas_width = 8;
	visible_overlay.canvas_height = 6;
	visible_overlay.target_x = 0;
	visible_overlay.target_y = 1;
	visible_overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceVisible;
	visible_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto actual_storage = RenderSecondaryOverlayScenario(storage_overlay);
	auto actual_visible = RenderSecondaryOverlayScenario(visible_overlay);
	auto storage_bounds = FindActiveBounds(actual_storage, 6, 8);
	auto visible_bounds = FindActiveBounds(actual_visible, 6, 8);
	auto full_parity_result = CompareRgbaImages("secondary/parity/full", actual_storage, actual_visible, 6, 8);
	auto stable_parity_result = CompareRgbaImagesOnStableMask(
		"secondary/parity/stable",
		actual_storage,
		actual_storage,
		actual_visible,
		6,
		8);
	std::cout
		<< "secondary/storage bounds="
		<< storage_bounds.x0 << "," << storage_bounds.y0 << " -> "
		<< storage_bounds.x1 << "," << storage_bounds.y1
		<< " alpha_pixels=" << storage_bounds.non_zero_alpha_pixels << "\n";
	std::cout
		<< "secondary/visible bounds="
		<< visible_bounds.x0 << "," << visible_bounds.y0 << " -> "
		<< visible_bounds.x1 << "," << visible_bounds.y1
		<< " alpha_pixels=" << visible_bounds.non_zero_alpha_pixels << "\n";
	PrintValidationResult(full_parity_result);
	PrintValidationResult(stable_parity_result);
	return storage_bounds.valid
		&& visible_bounds.valid
		&& storage_bounds.x0 == visible_bounds.x0
		&& storage_bounds.y0 == visible_bounds.y0
		&& storage_bounds.x1 == visible_bounds.x1
		&& storage_bounds.y1 == visible_bounds.y1
		&& storage_bounds.non_zero_alpha_pixels == visible_bounds.non_zero_alpha_pixels
		&& stable_parity_result.compared_pixels > 0
		&& stable_parity_result.max_abs_error == 0;
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	agi::log::log = new agi::log::LogSink;

	bool passed = true;
	passed = RunSecondaryOverlayTransformValidation() && passed;
	delete agi::log::log;
	agi::log::log = nullptr;
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "opengl-overlay-smoke failed: " << err.what() << std::endl;
	return 2;
}
catch (agi::Exception const& err) {
	std::cerr << "opengl-overlay-smoke failed: " << err.GetMessage() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "opengl-overlay-smoke failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "opengl-overlay-smoke is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
