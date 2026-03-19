#ifdef WITH_LIBPLACEBO

#include "../../src/source_frame.h"
#include "../../src/video_renderer_placebo_gl.h"

#include <libaegisub/exception.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef GetMessage
#undef GetMessage
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

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
using clock_type = std::chrono::steady_clock;
using WglCreateContextAttribsArbProc = HGLRC (WINAPI *)(HDC, HGLRC, int const*);
constexpr int kValidationBlockSize = 32;
constexpr int kValidationBoundaryMargin = 2;
constexpr int kChromaSitingStripeWidth = 16;

enum class YuvRangeMode {
	Limited,
	Full
};

struct Rgb8 {
	unsigned char r = 0;
	unsigned char g = 0;
	unsigned char b = 0;
};

struct QuantizedYuv {
	unsigned short y = 0;
	unsigned short u = 0;
	unsigned short v = 0;
};

struct ValidationResult {
	std::string name;
	int max_abs_error = 0;
	double mean_abs_error = 0.0;
	int max_x = 0;
	int max_y = 0;
	int max_channel = 0;
	std::array<int, 3> reference_rgb = { { 0, 0, 0 } };
	std::array<int, 3> candidate_rgb = { { 0, 0, 0 } };
};

struct BenchResult {
	std::string format;
	std::string phase;
	int width = 0;
	int height = 0;
	double wall_ns_per_frame = 0.0;
	double cpu_ns_per_frame = 0.0;
	double cpu_percent = 0.0;
	double wall_rel_bgra8 = 0.0;
	double upload_rel_bgra8 = 0.0;
	double upload_mib_per_s = 0.0;
	std::size_t source_bytes = 0;
	std::size_t upload_bytes = 0;
	std::size_t target_bytes = 0;
	std::size_t working_set_bytes = 0;
	std::size_t private_usage_bytes = 0;
};

struct ProcessSnapshot {
	std::uint64_t cpu_ns = 0;
	std::size_t working_set_bytes = 0;
	std::size_t private_usage_bytes = 0;
};

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubPlaceboRenderBenchWindow";
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
			throw std::runtime_error("RegisterClassA failed for hidden OpenGL bench window.");

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
			"Aegisub placebo render bench",
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
			throw std::runtime_error("CreateWindowExA failed for hidden OpenGL bench window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for hidden OpenGL bench window.");

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
			throw std::runtime_error("ChoosePixelFormat failed for hidden OpenGL bench window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for hidden OpenGL bench window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for hidden OpenGL bench window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for hidden OpenGL bench window.");

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
					throw std::runtime_error("wglMakeCurrent failed for modern hidden OpenGL bench window.");
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
			throw std::runtime_error("wglMakeCurrent failed for hidden OpenGL bench window.");
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

struct FrameScenario {
	std::string name;
	int width = 0;
	int height = 0;
	std::size_t source_bytes = 0;
	std::size_t upload_bytes = 0;
	VideoFrame bgra_storage;
	std::vector<unsigned char> plane0_u8;
	std::vector<unsigned char> plane1_u8;
	std::vector<unsigned char> plane2_u8;
	std::vector<unsigned short> plane0_u16;
	std::vector<unsigned short> plane1_u16;
	std::vector<unsigned short> plane2_u16;
	SourceFrame frame;
};

std::uint64_t FileTimeToUint64(FILETIME const& value) {
	ULARGE_INTEGER combined = {};
	combined.LowPart = value.dwLowDateTime;
	combined.HighPart = value.dwHighDateTime;
	return combined.QuadPart;
}

ProcessSnapshot TakeProcessSnapshot() {
	ProcessSnapshot snapshot;
	FILETIME creation = {};
	FILETIME exit = {};
	FILETIME kernel = {};
	FILETIME user = {};
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
		snapshot.cpu_ns = (FileTimeToUint64(kernel) + FileTimeToUint64(user)) * 100;
	}

	PROCESS_MEMORY_COUNTERS_EX memory = {};
	if (GetProcessMemoryInfo(
		GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
		sizeof(memory))) {
		snapshot.working_set_bytes = static_cast<std::size_t>(memory.WorkingSetSize);
		snapshot.private_usage_bytes = static_cast<std::size_t>(memory.PrivateUsage);
	}

	return snapshot;
}

Rgb8 PatternColorForPixel(int x, int y) {
	static constexpr std::array<Rgb8, 12> palette = {{
		{ 0, 0, 0 },
		{ 255, 255, 255 },
		{ 255, 0, 0 },
		{ 0, 255, 0 },
		{ 0, 0, 255 },
		{ 255, 255, 0 },
		{ 0, 255, 255 },
		{ 255, 0, 255 },
		{ 32, 32, 32 },
		{ 96, 144, 192 },
		{ 180, 128, 72 },
		{ 224, 208, 144 }
	}};

	int block_x = x / kValidationBlockSize;
	int block_y = y / kValidationBlockSize;
	return palette[static_cast<std::size_t>((block_x * 3 + block_y * 5) % static_cast<int>(palette.size()))];
}

bool IsValidationInteriorPixel(int x, int y) {
	int local_x = x % kValidationBlockSize;
	int local_y = y % kValidationBlockSize;
	return local_x >= kValidationBoundaryMargin
		&& local_x < kValidationBlockSize - kValidationBoundaryMargin
		&& local_y >= kValidationBoundaryMargin
		&& local_y < kValidationBlockSize - kValidationBoundaryMargin;
}

QuantizedYuv QuantizeBt709Limited(Rgb8 rgb, int bits_per_component) {
	double r = rgb.r / 255.0;
	double g = rgb.g / 255.0;
	double b = rgb.b / 255.0;
	double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	double cb = (b - y) / (2.0 * (1.0 - 0.0722)) + 0.5;
	double cr = (r - y) / (2.0 * (1.0 - 0.2126)) + 0.5;
	double full_scale = static_cast<double>((1 << bits_per_component) - 1);
	double y_offset = full_scale * (16.0 / 255.0);
	double y_scale = full_scale * (219.0 / 255.0);
	double c_offset = full_scale * (128.0 / 255.0);
	double c_scale = full_scale * (224.0 / 255.0);

	auto clamp_quantized = [full_scale](double value) -> unsigned short {
		value = std::round(value);
		value = std::max(0.0, std::min(full_scale, value));
		return static_cast<unsigned short>(value);
	};

	return {
		clamp_quantized(y_offset + y * y_scale),
		clamp_quantized(c_offset + (cb - 0.5) * c_scale),
		clamp_quantized(c_offset + (cr - 0.5) * c_scale)
	};
}

QuantizedYuv QuantizeBt709(Rgb8 rgb, int bits_per_component, YuvRangeMode range) {
	if (range == YuvRangeMode::Limited)
		return QuantizeBt709Limited(rgb, bits_per_component);

	double r = rgb.r / 255.0;
	double g = rgb.g / 255.0;
	double b = rgb.b / 255.0;
	double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	double cb = (b - y) / (2.0 * (1.0 - 0.0722)) + 0.5;
	double cr = (r - y) / (2.0 * (1.0 - 0.2126)) + 0.5;
	double full_scale = static_cast<double>((1 << bits_per_component) - 1);

	auto clamp_quantized = [full_scale](double value) -> unsigned short {
		value = std::round(value * full_scale);
		value = std::max(0.0, std::min(full_scale, value));
		return static_cast<unsigned short>(value);
	};

	return {
		clamp_quantized(y),
		clamp_quantized(cb),
		clamp_quantized(cr)
	};
}

FrameScenario MakeBgraScenario(int width, int height) {
	FrameScenario scenario;
	scenario.name = "bgra8";
	scenario.width = width;
	scenario.height = height;
	scenario.bgra_storage.width = static_cast<std::size_t>(width);
	scenario.bgra_storage.height = static_cast<std::size_t>(height);
	scenario.bgra_storage.pitch = static_cast<std::size_t>(width) * 4;
	scenario.bgra_storage.flipped = false;
	scenario.bgra_storage.data.resize(scenario.bgra_storage.pitch * scenario.bgra_storage.height);

	for (int y = 0; y < height; ++y) {
		auto* row = scenario.bgra_storage.data.data() + static_cast<std::size_t>(y) * scenario.bgra_storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto const color = PatternColorForPixel(x, y);
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = color.b;
			pixel[1] = color.g;
			pixel[2] = color.r;
			pixel[3] = 255;
		}
	}

	scenario.frame = MakeSourceFrameView(
		scenario.bgra_storage,
		SourceFrameColorMetadata { "RGB", "BT.709", "BT.1886", SourceFrameColorRange::Full });
	scenario.source_bytes = scenario.bgra_storage.data.size();
	scenario.upload_bytes = scenario.source_bytes;
	return scenario;
}

std::vector<unsigned char> MakeExpectedRgbaImage(int width, int height) {
	std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto const color = PatternColorForPixel(x, y);
			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			pixels[base + 0] = color.r;
			pixels[base + 1] = color.g;
			pixels[base + 2] = color.b;
			pixels[base + 3] = 255;
		}
	}
	return pixels;
}

std::vector<unsigned char> FlipRgbaTopLeftVertically(
	std::vector<unsigned char> const& pixels,
	int width,
	int height) {
	std::vector<unsigned char> flipped(pixels.size());
	std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
	for (int y = 0; y < height; ++y) {
		auto const* src = pixels.data() + static_cast<std::size_t>(y) * row_bytes;
		auto* dst = flipped.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes;
		std::memcpy(dst, src, row_bytes);
	}
	return flipped;
}

FrameScenario MakeYuv420p8Scenario(int width, int height) {
	FrameScenario scenario;
	scenario.name = "yuv420p8";
	scenario.width = width;
	scenario.height = height;
	scenario.plane0_u8.resize(static_cast<std::size_t>(width) * height);
	scenario.plane1_u8.resize(static_cast<std::size_t>(width / 2) * (height / 2));
	scenario.plane2_u8.resize(static_cast<std::size_t>(width / 2) * (height / 2));

	for (int y = 0; y < height; y += 2) {
		for (int x = 0; x < width; x += 2) {
			auto const color = PatternColorForPixel(x, y);
			auto const q = QuantizeBt709Limited(color, 8);
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					scenario.plane0_u8[static_cast<std::size_t>(y + dy) * width + (x + dx)] =
						static_cast<unsigned char>(q.y);
				}
			}
			scenario.plane1_u8[static_cast<std::size_t>(y / 2) * (width / 2) + (x / 2)] =
				static_cast<unsigned char>(q.u);
			scenario.plane2_u8[static_cast<std::size_t>(y / 2) * (width / 2) + (x / 2)] =
				static_cast<unsigned char>(q.v);
		}
	}

	scenario.frame.output_mode = SourceFrameOutputMode::Native;
	scenario.frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 3 };
	scenario.frame.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
	scenario.frame.width = width;
	scenario.frame.height = height;
	scenario.frame.plane_count = 3;
	scenario.frame.chroma_location = SourceFrameChromaLocation::Left;
	scenario.frame.planes[0] = { scenario.plane0_u8.data(), width, width, height };
	scenario.frame.planes[1] = { scenario.plane1_u8.data(), width / 2, width / 2, height / 2 };
	scenario.frame.planes[2] = { scenario.plane2_u8.data(), width / 2, width / 2, height / 2 };
	scenario.frame.color = { "BT.709", "BT.709", "BT.1886", SourceFrameColorRange::Limited };
	scenario.source_bytes = scenario.plane0_u8.size() + scenario.plane1_u8.size() + scenario.plane2_u8.size();
	scenario.upload_bytes = scenario.source_bytes;
	return scenario;
}

FrameScenario MakeYuv420p10Scenario(int width, int height) {
	FrameScenario scenario;
	scenario.name = "yuv420p10";
	scenario.width = width;
	scenario.height = height;
	scenario.plane0_u16.resize(static_cast<std::size_t>(width) * height);
	scenario.plane1_u16.resize(static_cast<std::size_t>(width / 2) * (height / 2));
	scenario.plane2_u16.resize(static_cast<std::size_t>(width / 2) * (height / 2));

	for (int y = 0; y < height; y += 2) {
		for (int x = 0; x < width; x += 2) {
			auto const color = PatternColorForPixel(x, y);
			auto const q = QuantizeBt709Limited(color, 10);
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					scenario.plane0_u16[static_cast<std::size_t>(y + dy) * width + (x + dx)] = q.y;
				}
			}
			scenario.plane1_u16[static_cast<std::size_t>(y / 2) * (width / 2) + (x / 2)] = q.u;
			scenario.plane2_u16[static_cast<std::size_t>(y / 2) * (width / 2) + (x / 2)] = q.v;
		}
	}

	scenario.frame.output_mode = SourceFrameOutputMode::Native;
	scenario.frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 4 };
	scenario.frame.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
	scenario.frame.width = width;
	scenario.frame.height = height;
	scenario.frame.plane_count = 3;
	scenario.frame.chroma_location = SourceFrameChromaLocation::Left;
	scenario.frame.planes[0] = {
		reinterpret_cast<unsigned char const*>(scenario.plane0_u16.data()),
		static_cast<ptrdiff_t>(width * 2),
		width,
		height
	};
	scenario.frame.planes[1] = {
		reinterpret_cast<unsigned char const*>(scenario.plane1_u16.data()),
		static_cast<ptrdiff_t>(width),
		width / 2,
		height / 2
	};
	scenario.frame.planes[2] = {
		reinterpret_cast<unsigned char const*>(scenario.plane2_u16.data()),
		static_cast<ptrdiff_t>(width),
		width / 2,
		height / 2
	};
	scenario.frame.color = { "BT.709", "BT.709", "BT.1886", SourceFrameColorRange::Limited };
	scenario.source_bytes = (scenario.plane0_u16.size() + scenario.plane1_u16.size() + scenario.plane2_u16.size()) * sizeof(unsigned short);
	scenario.upload_bytes = scenario.source_bytes;
	return scenario;
}

FrameScenario MakeP010Scenario(int width, int height) {
	FrameScenario scenario;
	scenario.name = "p010";
	scenario.width = width;
	scenario.height = height;
	scenario.plane0_u16.resize(static_cast<std::size_t>(width) * height);
	scenario.plane1_u16.resize(static_cast<std::size_t>(width) * (height / 2));

	for (int y = 0; y < height; y += 2) {
		for (int x = 0; x < width; x += 2) {
			auto const color = PatternColorForPixel(x, y);
			auto const q = QuantizeBt709Limited(color, 10);
			unsigned short y_shifted = static_cast<unsigned short>(q.y << 6);
			unsigned short u_shifted = static_cast<unsigned short>(q.u << 6);
			unsigned short v_shifted = static_cast<unsigned short>(q.v << 6);
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					scenario.plane0_u16[static_cast<std::size_t>(y + dy) * width + (x + dx)] = y_shifted;
				}
			}
			std::size_t uv_index = static_cast<std::size_t>(y / 2) * width + x;
			scenario.plane1_u16[uv_index + 0] = u_shifted;
			scenario.plane1_u16[uv_index + 1] = v_shifted;
		}
	}

	scenario.frame.output_mode = SourceFrameOutputMode::Native;
	scenario.frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 2 };
	scenario.frame.format_info = MakeSemiplanar420SourceFrameFormatInfo(
		10,
		2,
		4,
		{ { 6, 0, 0, 0 } },
		{ { 6, 22, 0, 0 } });
	scenario.frame.width = width;
	scenario.frame.height = height;
	scenario.frame.plane_count = 2;
	scenario.frame.chroma_location = SourceFrameChromaLocation::Left;
	scenario.frame.planes[0] = {
		reinterpret_cast<unsigned char const*>(scenario.plane0_u16.data()),
		static_cast<ptrdiff_t>(width * 2),
		width,
		height
	};
	scenario.frame.planes[1] = {
		reinterpret_cast<unsigned char const*>(scenario.plane1_u16.data()),
		static_cast<ptrdiff_t>(width * 2),
		width / 2,
		height / 2
	};
	scenario.frame.color = { "BT.709", "BT.709", "BT.1886", SourceFrameColorRange::Limited };
	scenario.source_bytes = (scenario.plane0_u16.size() + scenario.plane1_u16.size()) * sizeof(unsigned short);
	scenario.upload_bytes = scenario.source_bytes;
	return scenario;
}

FrameScenario MakeYuv420p8ChromaSitingScenario(
	int width,
	int height,
	SourceFrameChromaLocation chroma_location,
	YuvRangeMode range) {
	FrameScenario scenario;
	scenario.name = "yuv420p8-chroma-siting";
	scenario.width = width;
	scenario.height = height;
	auto neutral = QuantizeBt709({ 128, 128, 128 }, 8, range);
	scenario.plane0_u8.resize(static_cast<std::size_t>(width) * height, static_cast<unsigned char>(neutral.y));
	scenario.plane1_u8.resize(static_cast<std::size_t>(width / 2) * (height / 2));
	scenario.plane2_u8.resize(static_cast<std::size_t>(width / 2) * (height / 2));

	for (int y = 0; y < height / 2; ++y) {
		for (int x = 0; x < width / 2; ++x) {
			bool warm = ((x * 2) / kChromaSitingStripeWidth) % 2 == 0;
			auto q = QuantizeBt709(warm ? Rgb8{ 255, 96, 32 } : Rgb8{ 32, 160, 255 }, 8, range);
			scenario.plane1_u8[static_cast<std::size_t>(y) * (width / 2) + x] =
				static_cast<unsigned char>(q.u);
			scenario.plane2_u8[static_cast<std::size_t>(y) * (width / 2) + x] =
				static_cast<unsigned char>(q.v);
		}
	}

	scenario.frame.output_mode = SourceFrameOutputMode::Native;
	scenario.frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 3 };
	scenario.frame.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
	scenario.frame.width = width;
	scenario.frame.height = height;
	scenario.frame.plane_count = 3;
	scenario.frame.chroma_location = chroma_location;
	scenario.frame.planes[0] = { scenario.plane0_u8.data(), width, width, height };
	scenario.frame.planes[1] = { scenario.plane1_u8.data(), width / 2, width / 2, height / 2 };
	scenario.frame.planes[2] = { scenario.plane2_u8.data(), width / 2, width / 2, height / 2 };
	scenario.frame.color = {
		"BT.709",
		"BT.709",
		"BT.1886",
		range == YuvRangeMode::Limited ? SourceFrameColorRange::Limited : SourceFrameColorRange::Full
	};
	scenario.source_bytes = scenario.plane0_u8.size() + scenario.plane1_u8.size() + scenario.plane2_u8.size();
	scenario.upload_bytes = scenario.source_bytes;
	return scenario;
}

std::vector<unsigned char> RenderFrameToRgbaTopLeft(
	HiddenGLWindow& window,
	PlaceboRendererGL& renderer,
	SourceFrame const& frame,
	int width,
	int height) {
	window.MakeCurrent();
	glViewport(0, 0, width, height);
	renderer.UploadFrame(frame);
	renderer.UploadOverlay(nullptr);
	renderer.Render({ 0, 0, width, height }, width, height);
	return window.ReadBackRgbaTopLeft();
}

ValidationResult CompareRgbImages(
	std::string name,
	std::vector<unsigned char> const& reference,
	std::vector<unsigned char> const& candidate,
	int width,
	int height,
	bool interior_only = false) {
	ValidationResult result;
	result.name = std::move(name);

	std::uint64_t error_sum = 0;
	std::size_t pixel_count = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (interior_only && !IsValidationInteriorPixel(x, y))
				continue;
			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 3; ++channel) {
				int error = std::abs(
					static_cast<int>(reference[base + static_cast<std::size_t>(channel)])
					- static_cast<int>(candidate[base + static_cast<std::size_t>(channel)]));
				++pixel_count;
				error_sum += static_cast<std::uint64_t>(error);
				if (error > result.max_abs_error) {
					result.max_abs_error = error;
					result.max_x = x;
					result.max_y = y;
					result.max_channel = channel;
					result.reference_rgb = {
						static_cast<int>(reference[base + 0]),
						static_cast<int>(reference[base + 1]),
						static_cast<int>(reference[base + 2])
					};
					result.candidate_rgb = {
						static_cast<int>(candidate[base + 0]),
						static_cast<int>(candidate[base + 1]),
						static_cast<int>(candidate[base + 2])
					};
				}
			}
		}
	}

	result.mean_abs_error = pixel_count
		? static_cast<double>(error_sum) / static_cast<double>(pixel_count)
		: 0.0;
	return result;
}

void PrintValidationResult(ValidationResult const& result) {
	std::cout << std::left << std::setw(16) << result.name
		<< " max_abs=" << std::setw(3) << result.max_abs_error
		<< " mean_abs=" << std::fixed << std::setprecision(3) << result.mean_abs_error
		<< " worst=(" << result.max_x << "," << result.max_y << "," << result.max_channel << ")"
		<< " ref=(" << result.reference_rgb[0] << "," << result.reference_rgb[1] << "," << result.reference_rgb[2] << ")"
		<< " got=(" << result.candidate_rgb[0] << "," << result.candidate_rgb[1] << "," << result.candidate_rgb[2] << ")"
		<< "\n";
}

bool RunValidationSuite() {
	std::cout << "Validation against rendered BGRA8 baseline\n";

	auto bgra = MakeBgraScenario(128, 72);
	auto yuv420p8 = MakeYuv420p8Scenario(128, 72);
	auto yuv420p10 = MakeYuv420p10Scenario(128, 72);
	auto expected_pixels = MakeExpectedRgbaImage(128, 72);

	HiddenGLWindow window(128, 72);
	PlaceboRendererGL renderer;

	auto bgra_pixels = RenderFrameToRgbaTopLeft(window, renderer, bgra.frame, bgra.width, bgra.height);
	std::cout << "  rendered bgra8 baseline\n";
	auto yuv420p8_pixels = RenderFrameToRgbaTopLeft(window, renderer, yuv420p8.frame, yuv420p8.width, yuv420p8.height);
	std::cout << "  rendered yuv420p8\n";
	auto yuv420p10_pixels = RenderFrameToRgbaTopLeft(window, renderer, yuv420p10.frame, yuv420p10.width, yuv420p10.height);
	std::cout << "  rendered yuv420p10\n";

	std::array<ValidationResult, 3> results = {{
		CompareRgbImages("bgra8/full", expected_pixels, bgra_pixels, 128, 72),
		CompareRgbImages("yuv420p8/in", expected_pixels, yuv420p8_pixels, 128, 72, true),
		CompareRgbImages("yuv420p10/in", expected_pixels, yuv420p10_pixels, 128, 72, true)
	}};

	bool passed = true;
	for (auto const& result : results) {
		PrintValidationResult(result);
		if (result.max_abs_error > 4 || result.mean_abs_error > 1.0)
			passed = false;
	}

	return passed;
}

bool RunChromaLocationValidation() {
	std::cout << "\nValidation for chroma siting propagation\n";

	auto limited_left = MakeYuv420p8ChromaSitingScenario(128, 72, SourceFrameChromaLocation::Left, YuvRangeMode::Limited);
	auto limited_center = MakeYuv420p8ChromaSitingScenario(128, 72, SourceFrameChromaLocation::Center, YuvRangeMode::Limited);
	auto limited_unknown = MakeYuv420p8ChromaSitingScenario(128, 72, SourceFrameChromaLocation::Unknown, YuvRangeMode::Limited);
	auto full_left = MakeYuv420p8ChromaSitingScenario(128, 72, SourceFrameChromaLocation::Left, YuvRangeMode::Full);
	auto full_center = MakeYuv420p8ChromaSitingScenario(128, 72, SourceFrameChromaLocation::Center, YuvRangeMode::Full);
	auto full_unknown = MakeYuv420p8ChromaSitingScenario(128, 72, SourceFrameChromaLocation::Unknown, YuvRangeMode::Full);

	HiddenGLWindow window(128, 72);
	PlaceboRendererGL renderer;

	auto limited_left_pixels = RenderFrameToRgbaTopLeft(window, renderer, limited_left.frame, limited_left.width, limited_left.height);
	std::cout << "  rendered limited chroma-left sample\n";
	auto limited_center_pixels = RenderFrameToRgbaTopLeft(window, renderer, limited_center.frame, limited_center.width, limited_center.height);
	std::cout << "  rendered limited chroma-center sample\n";
	auto limited_unknown_pixels = RenderFrameToRgbaTopLeft(window, renderer, limited_unknown.frame, limited_unknown.width, limited_unknown.height);
	std::cout << "  rendered limited chroma-unknown sample\n";
	auto full_left_pixels = RenderFrameToRgbaTopLeft(window, renderer, full_left.frame, full_left.width, full_left.height);
	std::cout << "  rendered full chroma-left sample\n";
	auto full_center_pixels = RenderFrameToRgbaTopLeft(window, renderer, full_center.frame, full_center.width, full_center.height);
	std::cout << "  rendered full chroma-center sample\n";
	auto full_unknown_pixels = RenderFrameToRgbaTopLeft(window, renderer, full_unknown.frame, full_unknown.width, full_unknown.height);
	std::cout << "  rendered full chroma-unknown sample\n";

	auto limited_left_center = CompareRgbImages("lim-left-center", limited_left_pixels, limited_center_pixels, 128, 72);
	auto limited_left_unknown = CompareRgbImages("lim-left-unknown", limited_left_pixels, limited_unknown_pixels, 128, 72);
	auto full_left_unknown = CompareRgbImages("full-left-unknown", full_left_pixels, full_unknown_pixels, 128, 72);
	auto full_center_unknown = CompareRgbImages("full-center-unknown", full_center_pixels, full_unknown_pixels, 128, 72);
	PrintValidationResult(limited_left_center);
	PrintValidationResult(limited_left_unknown);
	PrintValidationResult(full_left_unknown);
	PrintValidationResult(full_center_unknown);

	return limited_left_center.max_abs_error >= 8
		&& limited_left_center.mean_abs_error >= 0.2
		&& limited_left_unknown.max_abs_error == 0
		&& limited_left_unknown.mean_abs_error == 0.0
		&& full_left_unknown.max_abs_error >= 8
		&& full_left_unknown.mean_abs_error >= 0.2
		&& full_center_unknown.max_abs_error == 0
		&& full_center_unknown.mean_abs_error == 0.0;
}

bool RunDisplayTransformValidation() {
	std::cout << "\nValidation for display_vflip transform\n";

	auto yuv420p8_unflipped = MakeYuv420p8Scenario(128, 72);
	auto yuv420p10_unflipped = MakeYuv420p10Scenario(128, 72);
	auto bgra = MakeBgraScenario(128, 72);
	bgra.frame.geometry.display_vflip = true;
	auto yuv420p8 = MakeYuv420p8Scenario(128, 72);
	yuv420p8.frame.geometry.display_vflip = true;
	auto yuv420p10 = MakeYuv420p10Scenario(128, 72);
	yuv420p10.frame.geometry.display_vflip = true;
	auto expected_bgra_pixels = MakeExpectedRgbaImage(128, 72);

	HiddenGLWindow window(128, 72);
	PlaceboRendererGL renderer;

	auto yuv420p8_unflipped_pixels = RenderFrameToRgbaTopLeft(
		window, renderer, yuv420p8_unflipped.frame, yuv420p8_unflipped.width, yuv420p8_unflipped.height);
	std::cout << "  rendered yuv420p8 baseline sample\n";
	auto yuv420p10_unflipped_pixels = RenderFrameToRgbaTopLeft(
		window, renderer, yuv420p10_unflipped.frame, yuv420p10_unflipped.width, yuv420p10_unflipped.height);
	std::cout << "  rendered yuv420p10 baseline sample\n";
	auto bgra_pixels = RenderFrameToRgbaTopLeft(window, renderer, bgra.frame, bgra.width, bgra.height);
	std::cout << "  rendered bgra8 baked-display sample\n";
	auto yuv420p8_pixels = RenderFrameToRgbaTopLeft(window, renderer, yuv420p8.frame, yuv420p8.width, yuv420p8.height);
	std::cout << "  rendered yuv420p8 display-vflip sample\n";
	auto yuv420p10_pixels = RenderFrameToRgbaTopLeft(window, renderer, yuv420p10.frame, yuv420p10.width, yuv420p10.height);
	std::cout << "  rendered yuv420p10 display-vflip sample\n";
	// Subsampled YUV cannot be validated against a vertically flipped "ideal RGB"
	// image because flipping after reconstruction shifts the effective chroma
	// sampling contract. Compare against the same-format unflipped render
	// baseline, then flip that resolved output in output space instead.
	auto expected_yuv420p8_flipped = FlipRgbaTopLeftVertically(yuv420p8_unflipped_pixels, 128, 72);
	auto expected_yuv420p10_flipped = FlipRgbaTopLeftVertically(yuv420p10_unflipped_pixels, 128, 72);

	std::array<ValidationResult, 3> results = {{
		// Legacy BGRA frames are already display-oriented before upload. Their
		// display geometry may still carry reference metadata, but placebo should
		// not apply rotation/vflip a second time on this path.
		CompareRgbImages("bgra8/baked", expected_bgra_pixels, bgra_pixels, 128, 72),
		CompareRgbImages("yuv420p8/vf", expected_yuv420p8_flipped, yuv420p8_pixels, 128, 72, true),
		CompareRgbImages("yuv420p10/vf", expected_yuv420p10_flipped, yuv420p10_pixels, 128, 72, true)
	}};

	bool passed = true;
	for (auto const& result : results) {
		PrintValidationResult(result);
		if (result.max_abs_error > 4 || result.mean_abs_error > 1.0)
			passed = false;
	}

	return passed;
}

enum class BenchPhase {
	UploadOnly,
	RenderOnly,
	UploadRender
};

char const* BenchPhaseName(BenchPhase phase) {
	switch (phase) {
		case BenchPhase::UploadOnly: return "upload_only";
		case BenchPhase::RenderOnly: return "render_only";
		case BenchPhase::UploadRender: return "upload_render";
		default: return "unknown";
	}
}

BenchResult RunBench(FrameScenario const& scenario, BenchPhase phase, std::size_t iterations) {
	HiddenGLWindow window(scenario.width, scenario.height);
	PlaceboRendererGL renderer;
	RenderViewport viewport { 0, 0, scenario.width, scenario.height };

	auto upload_only = [&]() {
		window.MakeCurrent();
		renderer.UploadFrame(scenario.frame);
		glFinish();
	};

	auto render_only = [&]() {
		window.MakeCurrent();
		glViewport(0, 0, scenario.width, scenario.height);
		renderer.Render(viewport, scenario.width, scenario.height);
		glFinish();
	};

	auto upload_render = [&]() {
		window.MakeCurrent();
		glViewport(0, 0, scenario.width, scenario.height);
		renderer.UploadFrame(scenario.frame);
		renderer.Render(viewport, scenario.width, scenario.height);
		glFinish();
	};

	if (phase == BenchPhase::RenderOnly) {
		upload_render();
	}
	else {
		for (int i = 0; i < 3; ++i) {
			if (phase == BenchPhase::UploadOnly)
				upload_only();
			else
				upload_render();
		}
	}

	auto steady_snapshot = TakeProcessSnapshot();

	for (int i = 0; i < 4; ++i) {
		if (phase == BenchPhase::UploadOnly)
			upload_only();
		else if (phase == BenchPhase::RenderOnly)
			render_only();
		else
			upload_render();
	}

	auto cpu_before = TakeProcessSnapshot();
	auto wall_start = clock_type::now();
	for (std::size_t i = 0; i < iterations; ++i) {
		if (phase == BenchPhase::UploadOnly)
			upload_only();
		else if (phase == BenchPhase::RenderOnly)
			render_only();
		else
			upload_render();
	}
	auto wall_end = clock_type::now();
	auto cpu_after = TakeProcessSnapshot();

	double wall_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
	double cpu_ns = static_cast<double>(cpu_after.cpu_ns - cpu_before.cpu_ns);

	BenchResult result;
	result.format = scenario.name;
	result.phase = BenchPhaseName(phase);
	result.width = scenario.width;
	result.height = scenario.height;
	result.wall_ns_per_frame = wall_ns / iterations;
	result.cpu_ns_per_frame = cpu_ns / iterations;
	result.cpu_percent = wall_ns > 0.0 ? cpu_ns / wall_ns * 100.0 : 0.0;
	result.source_bytes = scenario.source_bytes;
	result.upload_bytes = phase == BenchPhase::RenderOnly ? 0 : scenario.upload_bytes;
	result.target_bytes = phase == BenchPhase::UploadOnly
		? 0
		: static_cast<std::size_t>(scenario.width) * scenario.height * 4;
	result.working_set_bytes = steady_snapshot.working_set_bytes;
	result.private_usage_bytes = steady_snapshot.private_usage_bytes;
	if (result.wall_ns_per_frame > 0.0 && result.upload_bytes > 0) {
		double seconds_per_frame = result.wall_ns_per_frame / 1'000'000'000.0;
		result.upload_mib_per_s =
			static_cast<double>(result.upload_bytes) / seconds_per_frame / (1024.0 * 1024.0);
	}
	return result;
}

std::vector<BenchResult> RunBenchGroup(int width, int height, BenchPhase phase, std::size_t iterations) {
	auto bgra = MakeBgraScenario(width, height);
	auto yuv420p8 = MakeYuv420p8Scenario(width, height);
	auto yuv420p10 = MakeYuv420p10Scenario(width, height);

	std::vector<BenchResult> results;
	results.push_back(RunBench(bgra, phase, iterations));
	results.push_back(RunBench(yuv420p8, phase, iterations));
	results.push_back(RunBench(yuv420p10, phase, iterations));

	double bgra_wall = results.front().wall_ns_per_frame;
	double bgra_upload = static_cast<double>(std::max<std::size_t>(1, results.front().upload_bytes));
	for (auto& result : results) {
		result.wall_rel_bgra8 = bgra_wall > 0.0 ? result.wall_ns_per_frame / bgra_wall : 0.0;
		result.upload_rel_bgra8 = result.upload_bytes / bgra_upload;
	}

	return results;
}

void PrintBenchGroup(std::vector<BenchResult> const& results) {
	if (results.empty())
		return;

	std::cout << "\n"
		<< results.front().phase << " @ "
		<< results.front().width << "x" << results.front().height << "\n";
	std::cout << std::left << std::setw(12) << "format"
		<< std::right << std::setw(13) << "wall us/f"
		<< std::setw(10) << "rel"
		<< std::setw(13) << "cpu us/f"
		<< std::setw(10) << "cpu %"
		<< std::setw(12) << "src MiB"
		<< std::setw(12) << "upload"
		<< std::setw(10) << "u rel"
		<< std::setw(14) << "upl MiB/s"
		<< std::setw(12) << "target"
		<< std::setw(12) << "WS MiB"
		<< std::setw(12) << "Priv MiB"
		<< "\n";

	for (auto const& result : results) {
		std::cout << std::left << std::setw(12) << result.format
			<< std::right << std::setw(13) << std::fixed << std::setprecision(2) << result.wall_ns_per_frame / 1000.0
			<< std::setw(10) << std::fixed << std::setprecision(2) << result.wall_rel_bgra8
			<< std::setw(13) << std::fixed << std::setprecision(2) << result.cpu_ns_per_frame / 1000.0
			<< std::setw(10) << std::fixed << std::setprecision(1) << result.cpu_percent
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.source_bytes / (1024.0 * 1024.0)
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.upload_bytes / (1024.0 * 1024.0)
			<< std::setw(10) << std::fixed << std::setprecision(2) << result.upload_rel_bgra8
			<< std::setw(14) << std::fixed << std::setprecision(2) << result.upload_mib_per_s
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.target_bytes / (1024.0 * 1024.0)
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.working_set_bytes / (1024.0 * 1024.0)
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.private_usage_bytes / (1024.0 * 1024.0)
			<< "\n";
	}
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);

	if (!RunValidationSuite()) {
		std::cerr << "\nValidation failed: native render output drifted too far from rendered BGRA8 baseline.\n";
		return 3;
	}
	if (!RunChromaLocationValidation()) {
		std::cerr << "\nValidation failed: chroma siting propagation did not match expected libplacebo behavior.\n";
		return 4;
	}
	if (!RunDisplayTransformValidation()) {
		std::cerr << "\nValidation failed: display_vflip render output drifted from vertically flipped baseline.\n";
		return 5;
	}

	std::cout << "\nPlacebo renderer benchmark\n";
	std::cout << "Metrics:\n";
	std::cout << "  wall us/f  : steady-state wall time per frame\n";
	std::cout << "  cpu us/f   : process CPU time per frame\n";
	std::cout << "  cpu %      : process CPU / wall ratio for the timed section\n";
	std::cout << "  src MiB    : resident source-frame footprint per frame\n";
	std::cout << "  upload     : host-to-GPU upload payload per frame\n";
	std::cout << "  target     : output framebuffer write footprint proxy per frame\n";
	std::cout << "  WS/Priv MiB: process working-set/private-usage snapshot after warmup\n";

	PrintBenchGroup(RunBenchGroup(1920, 1080, BenchPhase::UploadOnly, 90));
	PrintBenchGroup(RunBenchGroup(1920, 1080, BenchPhase::RenderOnly, 150));
	PrintBenchGroup(RunBenchGroup(1920, 1080, BenchPhase::UploadRender, 60));
	PrintBenchGroup(RunBenchGroup(3840, 2160, BenchPhase::UploadRender, 18));
	return 0;
}
catch (std::exception const& err) {
	std::cerr << "placebo-render-bench failed: " << err.what() << std::endl;
	return 2;
}
catch (agi::Exception const& err) {
	std::cerr << "placebo-render-bench failed: " << err.GetMessage() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "placebo-render-bench failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "placebo-render-bench is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif

#else

int main() {
	return 0;
}

#endif
