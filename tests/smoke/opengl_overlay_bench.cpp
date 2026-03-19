#include "../../src/source_frame.h"
#include "../../src/subtitle_overlay.h"
#include "../../src/video_renderer_opengl.h"

#include <libaegisub/exception.h>
#include <libaegisub/log.h>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <eh.h>
#include <psapi.h>
#ifdef GetMessage
#undef GetMessage
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
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
using clock_type = std::chrono::steady_clock;
using WglCreateContextAttribsArbProc = HGLRC (WINAPI *)(HDC, HGLRC, int const*);

void SehTranslator(unsigned int code, EXCEPTION_POINTERS*) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "SEH exception 0x%08X", code);
	throw std::runtime_error(buffer);
}

struct ProcessSnapshot {
	std::uint64_t cpu_ns = 0;
	std::size_t working_set_bytes = 0;
	std::size_t private_usage_bytes = 0;
};

struct BenchResult {
	std::string scenario;
	std::string phase;
	int width = 0;
	int height = 0;
	double wall_ns_per_iter = 0.0;
	double cpu_ns_per_iter = 0.0;
	double cpu_percent = 0.0;
	double wall_rel_full = 0.0;
	double upload_rel_full = 0.0;
	double upload_mib_per_s = 0.0;
	std::size_t upload_bytes = 0;
	std::size_t working_set_bytes = 0;
	std::size_t private_usage_bytes = 0;
};

struct OverlayScenario {
	std::string name;
	int canvas_width = 0;
	int canvas_height = 0;
	std::size_t upload_bytes = 0;
	VideoFrame frame_storage;
	SubtitleOverlayStorage overlay_storage;
	SourceFrame frame;
	SubtitleOverlay overlay;
};

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubOpenGLOverlayBenchWindow";
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
			"Aegisub OpenGL overlay bench",
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
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
		snapshot.cpu_ns = (FileTimeToUint64(kernel) + FileTimeToUint64(user)) * 100;

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

OverlayScenario MakeFullFrameOverlayScenario(
	int canvas_width,
	int canvas_height,
	int patch_x,
	int patch_y,
	int patch_width,
	int patch_height) {
	OverlayScenario scenario;
	scenario.name = "full_frame";
	scenario.canvas_width = canvas_width;
	scenario.canvas_height = canvas_height;
	scenario.frame_storage.width = static_cast<std::size_t>(canvas_width);
	scenario.frame_storage.height = static_cast<std::size_t>(canvas_height);
	scenario.frame_storage.pitch = static_cast<std::size_t>(canvas_width) * 4;
	scenario.frame_storage.flipped = false;
	scenario.frame_storage.data.assign(scenario.frame_storage.pitch * scenario.frame_storage.height, 0);
	for (std::size_t i = 3; i < scenario.frame_storage.data.size(); i += 4)
		scenario.frame_storage.data[i] = 255;
	scenario.frame = MakeSourceFrameView(
		scenario.frame_storage,
		SourceFrameColorMetadata { "RGB", "BT.709", "BT.1886", SourceFrameColorRange::Full });

	scenario.overlay_storage.Reset(canvas_width, canvas_height, false);
	scenario.overlay_storage.has_visible_content = true;
	for (int y = 0; y < patch_height; ++y) {
		auto* row = scenario.overlay_storage.pixels.data()
			+ static_cast<std::size_t>(patch_y + y) * scenario.overlay_storage.pitch
			+ static_cast<std::size_t>(patch_x) * 4;
		for (int x = 0; x < patch_width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = 32;
			pixel[1] = 220;
			pixel[2] = 255;
			pixel[3] = 255;
		}
	}

	scenario.overlay = scenario.overlay_storage.MakeView(true);
	scenario.overlay.canvas_width = canvas_width;
	scenario.overlay.canvas_height = canvas_height;
	scenario.overlay.target_x = 0;
	scenario.overlay.target_y = 0;
	scenario.overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	scenario.overlay.force_full_upload = true;
	scenario.upload_bytes = static_cast<std::size_t>(canvas_width) * canvas_height * 4;
	return scenario;
}

OverlayScenario MakeSubviewOverlayScenario(
	int canvas_width,
	int canvas_height,
	int patch_x,
	int patch_y,
	int patch_width,
	int patch_height) {
	OverlayScenario scenario;
	scenario.name = "subview";
	scenario.canvas_width = canvas_width;
	scenario.canvas_height = canvas_height;
	scenario.frame_storage.width = static_cast<std::size_t>(canvas_width);
	scenario.frame_storage.height = static_cast<std::size_t>(canvas_height);
	scenario.frame_storage.pitch = static_cast<std::size_t>(canvas_width) * 4;
	scenario.frame_storage.flipped = false;
	scenario.frame_storage.data.assign(scenario.frame_storage.pitch * scenario.frame_storage.height, 0);
	for (std::size_t i = 3; i < scenario.frame_storage.data.size(); i += 4)
		scenario.frame_storage.data[i] = 255;
	scenario.frame = MakeSourceFrameView(
		scenario.frame_storage,
		SourceFrameColorMetadata { "RGB", "BT.709", "BT.1886", SourceFrameColorRange::Full });

	scenario.overlay_storage.Reset(patch_width, patch_height, false);
	scenario.overlay_storage.has_visible_content = true;
	for (int y = 0; y < patch_height; ++y) {
		auto* row = scenario.overlay_storage.pixels.data() + static_cast<std::size_t>(y) * scenario.overlay_storage.pitch;
		for (int x = 0; x < patch_width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = 32;
			pixel[1] = 220;
			pixel[2] = 255;
			pixel[3] = 255;
		}
	}

	scenario.overlay = scenario.overlay_storage.MakeView(true);
	scenario.overlay.canvas_width = canvas_width;
	scenario.overlay.canvas_height = canvas_height;
	scenario.overlay.target_x = patch_x;
	scenario.overlay.target_y = patch_y;
	scenario.overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	scenario.overlay.force_full_upload = true;
	scenario.upload_bytes = static_cast<std::size_t>(patch_width) * patch_height * 4;
	return scenario;
}

int CompareImagesMaxAbs(
	std::vector<unsigned char> const& lhs,
	std::vector<unsigned char> const& rhs) {
	if (lhs.size() != rhs.size())
		return 255;

	int max_abs = 0;
	for (size_t i = 0; i < lhs.size(); ++i)
		max_abs = std::max(max_abs, std::abs(static_cast<int>(lhs[i]) - static_cast<int>(rhs[i])));
	return max_abs;
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

int CompareImagesStableMaxAbs(
	std::vector<unsigned char> const& mask_source,
	std::vector<unsigned char> const& lhs,
	std::vector<unsigned char> const& rhs,
	int width,
	int height,
	int& compared_pixels) {
	if (mask_source.size() != lhs.size() || lhs.size() != rhs.size())
		return 255;

	int max_abs = 0;
	compared_pixels = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (!IsStablePixel(mask_source, width, height, x, y))
				continue;

			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				max_abs = std::max(
					max_abs,
					std::abs(static_cast<int>(lhs[base + channel]) - static_cast<int>(rhs[base + channel])));
				++compared_pixels;
			}
		}
	}
	return max_abs;
}

std::vector<unsigned char> RenderOverlayScenario(OverlayScenario const& scenario) {
	HiddenGLWindow window(scenario.canvas_width, scenario.canvas_height);
	OpenGLVideoRenderer renderer(true, true, true);
	window.MakeCurrent();
	renderer.UploadFrame(scenario.frame);
	renderer.UploadOverlay(&scenario.overlay);
	renderer.Render(
		{ 0, 0, scenario.canvas_width, scenario.canvas_height },
		scenario.canvas_width,
		scenario.canvas_height);
	auto pixels = window.ReadBackRgbaTopLeft();
	window.MakeCurrent();
	renderer.Reset();
	return pixels;
}

bool RunParityCheck() {
	int canvas_width = 640;
	int canvas_height = 360;
	int patch_width = 128;
	int patch_height = 48;
	int patch_x = 220;
	int patch_y = 260;

	auto full = MakeFullFrameOverlayScenario(
		canvas_width, canvas_height, patch_x, patch_y, patch_width, patch_height);
	auto subview = MakeSubviewOverlayScenario(
		canvas_width, canvas_height, patch_x, patch_y, patch_width, patch_height);

	auto full_pixels = RenderOverlayScenario(full);
	auto subview_pixels = RenderOverlayScenario(subview);
	int full_max_abs = CompareImagesMaxAbs(full_pixels, subview_pixels);
	int stable_compared_pixels = 0;
	int stable_max_abs = CompareImagesStableMaxAbs(
		full_pixels,
		full_pixels,
		subview_pixels,
		canvas_width,
		canvas_height,
		stable_compared_pixels);
	std::cout
		<< "overlay parity full_max_abs=" << full_max_abs
		<< " stable_max_abs=" << stable_max_abs
		<< " stable_pixels=" << stable_compared_pixels
		<< "\n";
	return stable_compared_pixels > 0 && stable_max_abs == 0;
}

enum class BenchPhase {
	UploadOnly,
	UploadRender
};

char const *BenchPhaseName(BenchPhase phase) {
	switch (phase) {
		case BenchPhase::UploadOnly: return "upload_only";
		case BenchPhase::UploadRender: return "upload_render";
		default: return "unknown";
	}
}

BenchResult RunBench(OverlayScenario const& scenario, BenchPhase phase, std::size_t iterations) {
	HiddenGLWindow window(scenario.canvas_width, scenario.canvas_height);
	OpenGLVideoRenderer renderer(true, true, true);
	window.MakeCurrent();

	auto run_once = [&]() {
		renderer.UploadOverlay(&scenario.overlay);
		if (phase == BenchPhase::UploadRender)
			renderer.Render(
				{ 0, 0, scenario.canvas_width, scenario.canvas_height },
				scenario.canvas_width,
				scenario.canvas_height);
		glFinish();
	};

	renderer.UploadFrame(scenario.frame);
	renderer.UploadOverlay(nullptr);
	renderer.Render(
		{ 0, 0, scenario.canvas_width, scenario.canvas_height },
		scenario.canvas_width,
		scenario.canvas_height);
	glFinish();

	for (int i = 0; i < 4; ++i)
		run_once();

	auto steady_snapshot = TakeProcessSnapshot();
	auto cpu_before = TakeProcessSnapshot();
	auto wall_start = clock_type::now();
	for (std::size_t i = 0; i < iterations; ++i)
		run_once();
	auto wall_end = clock_type::now();
	auto cpu_after = TakeProcessSnapshot();

	double wall_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
	double cpu_ns = static_cast<double>(cpu_after.cpu_ns - cpu_before.cpu_ns);

	window.MakeCurrent();
	renderer.Reset();

	BenchResult result;
	result.scenario = scenario.name;
	result.phase = BenchPhaseName(phase);
	result.width = scenario.canvas_width;
	result.height = scenario.canvas_height;
	result.wall_ns_per_iter = wall_ns / iterations;
	result.cpu_ns_per_iter = cpu_ns / iterations;
	result.cpu_percent = wall_ns > 0.0 ? cpu_ns / wall_ns * 100.0 : 0.0;
	result.upload_bytes = scenario.upload_bytes;
	result.working_set_bytes = steady_snapshot.working_set_bytes;
	result.private_usage_bytes = steady_snapshot.private_usage_bytes;
	if (result.wall_ns_per_iter > 0.0 && result.upload_bytes > 0) {
		double seconds_per_iter = result.wall_ns_per_iter / 1'000'000'000.0;
		result.upload_mib_per_s =
			static_cast<double>(result.upload_bytes) / seconds_per_iter / (1024.0 * 1024.0);
	}
	return result;
}

std::vector<BenchResult> RunBenchGroup(
	int canvas_width,
	int canvas_height,
	int patch_width,
	int patch_height,
	std::size_t upload_iterations,
	std::size_t upload_render_iterations) {
	int patch_x = (canvas_width - patch_width) / 2;
	int patch_y = canvas_height - patch_height - canvas_height / 12;

	auto full = MakeFullFrameOverlayScenario(
		canvas_width, canvas_height, patch_x, patch_y, patch_width, patch_height);
	auto subview = MakeSubviewOverlayScenario(
		canvas_width, canvas_height, patch_x, patch_y, patch_width, patch_height);

	std::vector<BenchResult> results;
	results.push_back(RunBench(full, BenchPhase::UploadOnly, upload_iterations));
	results.push_back(RunBench(subview, BenchPhase::UploadOnly, upload_iterations));
	results.push_back(RunBench(full, BenchPhase::UploadRender, upload_render_iterations));
	results.push_back(RunBench(subview, BenchPhase::UploadRender, upload_render_iterations));

	for (size_t i = 0; i < results.size(); i += 2) {
		double full_wall = results[i].wall_ns_per_iter;
		double full_upload = static_cast<double>(std::max<std::size_t>(1, results[i].upload_bytes));
		results[i].wall_rel_full = 1.0;
		results[i].upload_rel_full = 1.0;
		results[i + 1].wall_rel_full = full_wall > 0.0 ? results[i + 1].wall_ns_per_iter / full_wall : 0.0;
		results[i + 1].upload_rel_full = results[i + 1].upload_bytes / full_upload;
	}

	return results;
}

void PrintBenchGroup(std::vector<BenchResult> const& results) {
	if (results.empty())
		return;

	std::cout << "\nOverlay bench @ " << results.front().width << "x" << results.front().height << "\n";
	std::cout << std::left << std::setw(14) << "scenario"
		<< std::setw(14) << "phase"
		<< std::right << std::setw(12) << "wall us"
		<< std::setw(10) << "rel"
		<< std::setw(12) << "cpu us"
		<< std::setw(10) << "cpu %"
		<< std::setw(12) << "upload"
		<< std::setw(10) << "u rel"
		<< std::setw(14) << "upl MiB/s"
		<< std::setw(12) << "WS MiB"
		<< std::setw(12) << "Priv MiB"
		<< "\n";

	for (auto const& result : results) {
		std::cout << std::left << std::setw(14) << result.scenario
			<< std::setw(14) << result.phase
			<< std::right << std::setw(12) << std::fixed << std::setprecision(2) << result.wall_ns_per_iter / 1000.0
			<< std::setw(10) << std::fixed << std::setprecision(2) << result.wall_rel_full
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.cpu_ns_per_iter / 1000.0
			<< std::setw(10) << std::fixed << std::setprecision(1) << result.cpu_percent
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.upload_bytes / (1024.0 * 1024.0)
			<< std::setw(10) << std::fixed << std::setprecision(2) << result.upload_rel_full
			<< std::setw(14) << std::fixed << std::setprecision(2) << result.upload_mib_per_s
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.working_set_bytes / (1024.0 * 1024.0)
			<< std::setw(12) << std::fixed << std::setprecision(2) << result.private_usage_bytes / (1024.0 * 1024.0)
			<< "\n";
	}
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	agi::log::log = new agi::log::LogSink;

	if (!RunParityCheck()) {
		delete agi::log::log;
		agi::log::log = nullptr;
		std::cerr << "opengl-overlay-bench failed: full-frame and subview overlays rendered different pixels.\n";
		return 3;
	}

	std::cout << "Overlay upload bench\n";
	std::cout << "  full_frame: legacy-sized upload surface\n";
	std::cout << "  subview   : cropped upload surface with identical final pixels\n";

	PrintBenchGroup(RunBenchGroup(1920, 1080, 384, 96, 180, 120));
	PrintBenchGroup(RunBenchGroup(3840, 2160, 768, 192, 48, 32));

	delete agi::log::log;
	agi::log::log = nullptr;
	return 0;
}
catch (std::exception const& err) {
	std::cerr << "opengl-overlay-bench failed: " << err.what() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "opengl-overlay-bench failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "opengl-overlay-bench is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
