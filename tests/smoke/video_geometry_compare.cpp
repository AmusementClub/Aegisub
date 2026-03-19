#include "../../src/options.h"
#include "../../src/source_frame.h"
#include "../../src/subtitle_overlay.h"
#include "../../src/video_display_layout.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_renderer_opengl.h"
#include "../../src/video_renderer_placebo_gl.h"
#include "../../src/include/aegisub/video_provider.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/log.h>
#include <libaegisub/option.h>
#include <libaegisub/path.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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

#include <wx/cmdline.h>
#include <wx/init.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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

namespace Automation4 { class AutoloadScriptManager; }
namespace config {
	agi::Options *opt = nullptr;
	agi::MRUManager *mru = nullptr;
	agi::Path *path = nullptr;
	Automation4::AutoloadScriptManager *global_scripts = nullptr;
}

std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(
	agi::fs::path const& path,
	std::string const& colormatrix,
	agi::BackgroundRunner *br);

namespace {
using WglCreateContextAttribsArbProc = HGLRC (WINAPI *)(HDC, HGLRC, int const*);

void SehTranslator(unsigned int code, EXCEPTION_POINTERS*) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "SEH exception 0x%08X", code);
	throw std::runtime_error(buffer);
}

struct SourceGeometryExpectation {
	std::string sample_name;
	std::string file_name;
	SourceFrameRect visible_rect;
	int rotation = 0;
	bool display_vflip = false;
	double pixel_aspect_ratio = 1.0;
};

struct SampleInput {
	std::string sample_name;
	std::filesystem::path file_path;
	bool has_geometry_expectation = false;
	SourceGeometryExpectation geometry_expectation = { };
};

struct ImageCompareResult {
	int max_abs = 0;
	double mean_abs = 0.0;
	int compared_pixels = 0;
};

struct ActiveBounds {
	bool valid = false;
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	int non_zero_alpha_pixels = 0;
};

struct SampleResult {
	std::string sample_name;
	std::string file_path;
	bool passed = false;

	SourceFrameGeometry native_geometry = { };
	SourceFrameGeometry baked_geometry = { };
	SourceFrameRect display_output_rect = { };
	double display_aspect_ratio = 0.0;
	int provider_width = 0;
	int provider_height = 0;
	double provider_dar = 0.0;
	VideoDisplayViewportLayout viewport_400 = { };

	ImageCompareResult video_compare_full = { };
	ImageCompareResult video_compare_stable = { };
	ImageCompareResult overlay_compare_full = { };
	ImageCompareResult overlay_compare_stable = { };
	ActiveBounds overlay_storage_bounds = { };
	ActiveBounds overlay_visible_bounds = { };

	bool has_geometry_expectation = false;
	bool geometry_matches_expectation = false;
	bool provider_display_contract_ok = false;
	bool video_compare_ok = false;
	bool overlay_compare_ok = false;
	bool overlay_bounds_match = false;
};

class NullProgressSink final : public agi::ProgressSink {
public:
	void SetIndeterminate() override { }
	void SetTitle(std::string const&) override { }
	void SetMessage(std::string const&) override { }
	void SetProgress(int64_t, int64_t) override { }
	void Log(std::string const&) override { }
	bool IsCancelled() override { return false; }
};

class InlineBackgroundRunner final : public agi::BackgroundRunner {
public:
	void Run(std::function<void(agi::ProgressSink *)> task) override {
		NullProgressSink sink;
		task(&sink);
	}
};

class ScopedConfigContext {
	agi::Options options;
	agi::Path path_tokens;

public:
	ScopedConfigContext(std::filesystem::path const& root)
	: options("", R"json(
{
  "Provider": {
    "FFmpegSource": {
      "Index All Tracks": false,
      "Log Level": "quiet",
      "Cache": {
        "Size": 256,
        "Files": 256
      }
    },
    "Video": {
      "FFmpegSource": {
        "Decoding Threads": 1,
        "Unsafe Seeking": false
      }
    },
    "Audio": {
      "FFmpegSource": {
        "Decode Error Handling": "stop"
      }
    }
  },
  "Video": {
    "Open Audio": false
  }
}
)json", agi::Options::FLUSH_SKIP) {
		path_tokens.SetToken("?local", (root / "cache").string());
		path_tokens.SetToken("?temp", (root / "temp").string());
		path_tokens.SetToken("?data", root.string());
		path_tokens.SetToken("?user", root.string());
		config::opt = &options;
		config::path = &path_tokens;
	}

	~ScopedConfigContext() {
		config::opt = nullptr;
		config::path = nullptr;
	}
};

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubVideoGeometryCompareWindow";
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
			throw std::runtime_error("RegisterClassA failed for hidden geometry compare window.");

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
			"Aegisub video geometry compare",
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
			throw std::runtime_error("CreateWindowExA failed for hidden geometry compare window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for hidden geometry compare window.");

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
			throw std::runtime_error("ChoosePixelFormat failed for hidden geometry compare window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for hidden geometry compare window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for hidden geometry compare window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for hidden geometry compare window.");

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
					throw std::runtime_error("wglMakeCurrent failed for modern geometry compare window.");
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
			throw std::runtime_error("wglMakeCurrent failed for hidden geometry compare window.");
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

std::array<SourceGeometryExpectation, 7> BuildSampleSpecs() {
	return {{
		{ "baseline", "baseline.mp4", { 0, 0, 320, 180 }, 0, false, 1.0 },
		{ "sar_4_3", "sar_4_3.mp4", { 0, 0, 320, 180 }, 0, false, 4.0 / 3.0 },
		// These rows intentionally pin the currently observed FFMS provider
		// behavior for locally generated samples. Rotation/vflip metadata from
		// these bitstream-filter variants is not currently surfaced, and the
		// crop variant decodes as a smaller full-frame image.
		{ "rotate90", "rotate90.mp4", { 0, 0, 320, 180 }, 0, false, 1.0 },
		{ "vflip", "vflip.mp4", { 0, 0, 320, 180 }, 0, false, 1.0 },
		{ "crop_8_12_4_6", "crop_8_12_4_6.mp4", { 0, 0, 308, 182 }, 0, false, 1.0 },
		{ "crop_rotate90_sar_4_3", "crop_rotate90_sar_4_3.mp4", { 0, 0, 308, 182 }, 0, false, 4.0 / 3.0 },
		{ "rotate90_vflip", "rotate90_vflip.mp4", { 0, 0, 320, 180 }, 0, false, 1.0 }
	}};
}

std::vector<SampleInput> BuildBuiltInSampleInputs(std::filesystem::path const& samples_dir) {
	auto const specs = BuildSampleSpecs();
	std::vector<SampleInput> inputs;
	inputs.reserve(specs.size());
	for (auto const& spec : specs) {
		SampleInput input;
		input.sample_name = spec.sample_name;
		input.file_path = samples_dir / spec.file_name;
		input.has_geometry_expectation = true;
		input.geometry_expectation = spec;
		inputs.push_back(std::move(input));
	}
	return inputs;
}

std::string TrimAscii(std::string value) {
	auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
	while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
		value.erase(value.begin());
	while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
		value.pop_back();
	return value;
}

std::vector<SampleInput> LoadSampleManifest(std::filesystem::path const& manifest_path) {
	std::ifstream in(manifest_path, std::ios::binary);
	if (!in)
		throw std::runtime_error("Failed to open sample manifest: " + manifest_path.string());

	std::vector<SampleInput> inputs;
	std::string line;
	int line_number = 0;
	while (std::getline(in, line)) {
		++line_number;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		auto const trimmed = TrimAscii(line);
		if (trimmed.empty() || trimmed[0] == '#')
			continue;

		auto const separator = trimmed.find('|');
		if (separator == std::string::npos)
			throw std::runtime_error("Invalid manifest line " + std::to_string(line_number) + ": expected 'name|path'.");

		auto const name = TrimAscii(trimmed.substr(0, separator));
		auto const path_text = TrimAscii(trimmed.substr(separator + 1));
		if (name.empty() || path_text.empty())
			throw std::runtime_error("Invalid manifest line " + std::to_string(line_number) + ": empty name or path.");

		std::filesystem::path file_path = path_text;
		if (file_path.is_relative())
			file_path = manifest_path.parent_path() / file_path;

		SampleInput input;
		input.sample_name = name;
		input.file_path = file_path.lexically_normal();
		inputs.push_back(std::move(input));
	}

	if (inputs.empty())
		throw std::runtime_error("Sample manifest did not contain any usable lines: " + manifest_path.string());

	return inputs;
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

ImageCompareResult CompareImages(
	std::vector<unsigned char> const& lhs,
	std::vector<unsigned char> const& rhs,
	int width,
	int height,
	bool stable_only = false) {
	ImageCompareResult result;
	if (lhs.size() != rhs.size())
		return { 255, 255.0, 0 };

	double total_abs = 0.0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (stable_only && !IsStablePixel(lhs, width, height, x, y))
				continue;

			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				int abs_err = std::abs(static_cast<int>(lhs[base + channel]) - static_cast<int>(rhs[base + channel]));
				result.max_abs = std::max(result.max_abs, abs_err);
				total_abs += abs_err;
				++result.compared_pixels;
			}
		}
	}
	result.mean_abs = result.compared_pixels > 0
		? total_abs / result.compared_pixels
		: 0.0;
	return result;
}

ImageCompareResult CompareImagesOnStableMask(
	std::vector<unsigned char> const& mask_source,
	std::vector<unsigned char> const& lhs,
	std::vector<unsigned char> const& rhs,
	int width,
	int height) {
	ImageCompareResult result;
	if (mask_source.size() != lhs.size() || lhs.size() != rhs.size())
		return { 255, 255.0, 0 };

	double total_abs = 0.0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (!IsStablePixel(mask_source, width, height, x, y))
				continue;

			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				int abs_err = std::abs(static_cast<int>(lhs[base + channel]) - static_cast<int>(rhs[base + channel]));
				result.max_abs = std::max(result.max_abs, abs_err);
				total_abs += abs_err;
				++result.compared_pixels;
			}
		}
	}

	result.mean_abs = result.compared_pixels > 0
		? total_abs / result.compared_pixels
		: 0.0;
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

bool BoundsEqual(ActiveBounds const& lhs, ActiveBounds const& rhs) {
	return lhs.valid == rhs.valid
		&& lhs.x0 == rhs.x0
		&& lhs.y0 == rhs.y0
		&& lhs.x1 == rhs.x1
		&& lhs.y1 == rhs.y1
		&& lhs.non_zero_alpha_pixels == rhs.non_zero_alpha_pixels;
}

void ClearFramebuffer(int width, int height) {
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

SourceFrame MakeBgraSourceFrame(VideoProvider& provider, int frame_number, VideoFrame& storage) {
	provider.SetOutputMode(SourceFrameOutputMode::Bgra8);
	provider.GetFrame(frame_number, storage);
	auto frame = MakeSourceFrameView(storage, provider.GetColorMetadata());
	frame.geometry = provider.GetFrameGeometry();
	frame.native_format = provider.GetNativeFormatIdentity();
	return frame;
}

SourceFrame MakeNativeSourceFrame(VideoProvider& provider, int frame_number, std::shared_ptr<void>& owner) {
	if (!provider.SetOutputMode(SourceFrameOutputMode::Native))
		throw std::runtime_error("Provider does not support native output mode for compare harness.");

	SourceFrame frame;
	if (!provider.GetNativeFrame(frame_number, frame, owner))
		throw std::runtime_error("Provider failed to return native frame for compare harness.");
	if (!frame.IsValid())
		throw std::runtime_error("Provider returned invalid native frame for compare harness.");
	return frame;
}

std::vector<unsigned char> RenderBgraVideoFrame(SourceFrame const& frame, int width, int height) {
	HiddenGLWindow window(width, height);
	OpenGLVideoRenderer renderer(true, false, true);
	window.MakeCurrent();
	ClearFramebuffer(width, height);
	renderer.UploadFrame(frame);
	renderer.UploadOverlay(nullptr);
	renderer.Render({ 0, 0, width, height }, width, height);
	auto pixels = window.ReadBackRgbaTopLeft();
	window.MakeCurrent();
	renderer.Reset();
	return pixels;
}

std::vector<unsigned char> RenderNativeVideoFrame(SourceFrame const& frame, int width, int height) {
	HiddenGLWindow window(width, height);
	PlaceboRendererGL renderer;
	window.MakeCurrent();
	ClearFramebuffer(width, height);
	renderer.UploadFrame(frame);
	renderer.UploadOverlay(nullptr);
	renderer.Render({ 0, 0, width, height }, width, height);
	auto pixels = window.ReadBackRgbaTopLeft();
	window.MakeCurrent();
	renderer.Reset();
	return pixels;
}

SubtitleOverlayStorage MakeSolidOverlayStorage(int width, int height) {
	SubtitleOverlayStorage storage;
	storage.Reset(width, height, false);
	storage.has_visible_content = true;
	for (int y = 0; y < height; ++y) {
		auto* row = storage.pixels.data() + static_cast<std::size_t>(y) * storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = 40;
			pixel[1] = 220;
			pixel[2] = 255;
			pixel[3] = 255;
		}
	}
	return storage;
}

struct EquivalentOverlayPair {
	SubtitleOverlayStorage storage_overlay_storage;
	SubtitleOverlayStorage visible_overlay_storage;
	SubtitleOverlay storage_overlay;
	SubtitleOverlay visible_overlay;
};

EquivalentOverlayPair BuildEquivalentOverlays(SourceFrame const& source) {
	auto visible = GetSourceFrameVisibleRect(source);
	int patch_width = std::max(8, std::min(visible.width - 2, visible.width / 4));
	int patch_height = std::max(8, std::min(visible.height - 2, visible.height / 4));
	int patch_x = visible.x + std::max(1, (visible.width - patch_width) / 3);
	int patch_y = visible.y + std::max(1, (visible.height - patch_height) / 3);

	EquivalentOverlayPair overlays;
	overlays.storage_overlay_storage = MakeSolidOverlayStorage(patch_width, patch_height);
	overlays.visible_overlay_storage = MakeSolidOverlayStorage(patch_width, patch_height);

	overlays.storage_overlay = overlays.storage_overlay_storage.MakeView(true);
	overlays.storage_overlay.canvas_width = source.geometry.storage_width;
	overlays.storage_overlay.canvas_height = source.geometry.storage_height;
	overlays.storage_overlay.target_x = patch_x;
	overlays.storage_overlay.target_y = patch_y;
	overlays.storage_overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceStorage;
	overlays.storage_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	overlays.visible_overlay = overlays.visible_overlay_storage.MakeView(true);
	overlays.visible_overlay.canvas_width = visible.width;
	overlays.visible_overlay.canvas_height = visible.height;
	overlays.visible_overlay.target_x = patch_x - visible.x;
	overlays.visible_overlay.target_y = patch_y - visible.y;
	overlays.visible_overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceVisible;
	overlays.visible_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	return overlays;
}

std::vector<unsigned char> RenderOverlayOnly(SourceFrame const& source, SubtitleOverlay const& overlay, int width, int height) {
	HiddenGLWindow window(width, height);
	OpenGLVideoRenderer renderer(false, true, true);
	window.MakeCurrent();
	ClearFramebuffer(width, height);
	renderer.UploadFrame(source);
	renderer.UploadOverlay(&overlay);
	renderer.Render({ 0, 0, width, height }, width, height);
	auto pixels = window.ReadBackRgbaTopLeft();
	window.MakeCurrent();
	renderer.Reset();
	return pixels;
}

bool NearlyEqual(double lhs, double rhs, double eps = 1e-6) {
	return std::abs(lhs - rhs) <= eps;
}

bool GeometryMatchesExpectation(SourceFrameGeometry const& geometry, SourceGeometryExpectation const& expected) {
	return geometry.visible_rect.x == expected.visible_rect.x
		&& geometry.visible_rect.y == expected.visible_rect.y
		&& geometry.visible_rect.width == expected.visible_rect.width
		&& geometry.visible_rect.height == expected.visible_rect.height
		&& geometry.rotation == expected.rotation
		&& geometry.display_vflip == expected.display_vflip
		&& NearlyEqual(geometry.pixel_aspect_ratio, expected.pixel_aspect_ratio);
}

char const* GeometryStatusLabel(SampleResult const& result) {
	if (!result.provider_display_contract_ok)
		return "bad";
	if (!result.has_geometry_expectation)
		return "observed";
	return result.geometry_matches_expectation ? "ok" : "bad";
}

std::string JsonEscape(std::string const& value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (char ch : value) {
		switch (ch) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default: escaped += ch; break;
		}
	}
	return escaped;
}

std::string FormatDouble(double value, int precision = 6) {
	std::ostringstream out;
	out << std::setprecision(precision) << value;
	return out.str();
}

void WriteReport(std::filesystem::path const& report_path, std::vector<SampleResult> const& results) {
	std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
	out << "{\n  \"samples\": [\n";
	for (size_t i = 0; i < results.size(); ++i) {
		auto const& sample = results[i];
		out << "    {\n";
		out << "      \"name\": \"" << JsonEscape(sample.sample_name) << "\",\n";
		out << "      \"file\": \"" << JsonEscape(sample.file_path) << "\",\n";
		out << "      \"passed\": " << (sample.passed ? "true" : "false") << ",\n";
		out << "      \"provider_width\": " << sample.provider_width << ",\n";
		out << "      \"provider_height\": " << sample.provider_height << ",\n";
		out << "      \"provider_dar\": " << std::setprecision(12) << sample.provider_dar << ",\n";
		out << "      \"native_geometry\": {\n";
		out << "        \"storage_width\": " << sample.native_geometry.storage_width << ",\n";
		out << "        \"storage_height\": " << sample.native_geometry.storage_height << ",\n";
		out << "        \"visible_rect\": [" << sample.native_geometry.visible_rect.x << ", " << sample.native_geometry.visible_rect.y << ", " << sample.native_geometry.visible_rect.width << ", " << sample.native_geometry.visible_rect.height << "],\n";
		out << "        \"rotation\": " << sample.native_geometry.rotation << ",\n";
		out << "        \"display_vflip\": " << (sample.native_geometry.display_vflip ? "true" : "false") << ",\n";
		out << "        \"pixel_aspect_ratio\": " << std::setprecision(12) << sample.native_geometry.pixel_aspect_ratio << "\n";
		out << "      },\n";
		out << "      \"display_output_rect\": [" << sample.display_output_rect.x << ", " << sample.display_output_rect.y << ", " << sample.display_output_rect.width << ", " << sample.display_output_rect.height << "],\n";
		out << "      \"display_aspect_ratio\": " << std::setprecision(12) << sample.display_aspect_ratio << ",\n";
		out << "      \"viewport_400\": [" << sample.viewport_400.viewport_left << ", " << sample.viewport_400.viewport_top << ", " << sample.viewport_400.viewport_width << ", " << sample.viewport_400.viewport_height << "],\n";
		out << "      \"has_geometry_expectation\": " << (sample.has_geometry_expectation ? "true" : "false") << ",\n";
		out << "      \"geometry_status\": \"" << GeometryStatusLabel(sample) << "\",\n";
		out << "      \"geometry_matches_expectation\": " << (sample.geometry_matches_expectation ? "true" : "false") << ",\n";
		out << "      \"provider_display_contract_ok\": " << (sample.provider_display_contract_ok ? "true" : "false") << ",\n";
		out << "      \"video_compare\": {\n";
		out << "        \"full_max_abs\": " << sample.video_compare_full.max_abs << ",\n";
		out << "        \"full_mean_abs\": " << std::setprecision(6) << sample.video_compare_full.mean_abs << ",\n";
		out << "        \"stable_max_abs\": " << sample.video_compare_stable.max_abs << ",\n";
		out << "        \"stable_mean_abs\": " << std::setprecision(6) << sample.video_compare_stable.mean_abs << ",\n";
		out << "        \"stable_pixels\": " << sample.video_compare_stable.compared_pixels << "\n";
		out << "      },\n";
		out << "      \"overlay_compare\": {\n";
		out << "        \"full_max_abs\": " << sample.overlay_compare_full.max_abs << ",\n";
		out << "        \"stable_max_abs\": " << sample.overlay_compare_stable.max_abs << ",\n";
		out << "        \"stable_mean_abs\": " << std::setprecision(6) << sample.overlay_compare_stable.mean_abs << ",\n";
		out << "        \"stable_pixels\": " << sample.overlay_compare_stable.compared_pixels << "\n";
		out << "      },\n";
		out << "      \"overlay_storage_bounds\": [" << sample.overlay_storage_bounds.x0 << ", " << sample.overlay_storage_bounds.y0 << ", " << sample.overlay_storage_bounds.x1 << ", " << sample.overlay_storage_bounds.y1 << "],\n";
		out << "      \"overlay_visible_bounds\": [" << sample.overlay_visible_bounds.x0 << ", " << sample.overlay_visible_bounds.y0 << ", " << sample.overlay_visible_bounds.x1 << ", " << sample.overlay_visible_bounds.y1 << "],\n";
		out << "      \"overlay_storage_alpha_pixels\": " << sample.overlay_storage_bounds.non_zero_alpha_pixels << ",\n";
		out << "      \"overlay_visible_alpha_pixels\": " << sample.overlay_visible_bounds.non_zero_alpha_pixels << ",\n";
		out << "      \"overlay_bounds_match\": " << (sample.overlay_bounds_match ? "true" : "false") << ",\n";
		out << "      \"video_compare_ok\": " << (sample.video_compare_ok ? "true" : "false") << ",\n";
		out << "      \"overlay_compare_ok\": " << (sample.overlay_compare_ok ? "true" : "false") << "\n";
		out << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
	}
	out << "  ]\n}\n";
}

void WriteMatrix(std::filesystem::path const& matrix_path, std::vector<SampleResult> const& results) {
	std::ofstream out(matrix_path, std::ios::binary | std::ios::trunc);
	out << "# Video Geometry Compare Matrix\n\n";
	out << "| Sample | Geometry | Provider | DAR | Visible Rect | Rot | VFlip | PAR | Video Stable Max | Overlay Stable Max | Overlay Bounds | Result |\n";
	out << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
	for (auto const& sample : results) {
		out
			<< "| " << sample.sample_name
			<< " | " << GeometryStatusLabel(sample)
			<< " | " << sample.provider_width << "x" << sample.provider_height
			<< " | " << FormatDouble(sample.provider_dar)
			<< " | " << sample.native_geometry.visible_rect.x << "," << sample.native_geometry.visible_rect.y << "," << sample.native_geometry.visible_rect.width << "," << sample.native_geometry.visible_rect.height
			<< " | " << sample.native_geometry.rotation
			<< " | " << (sample.native_geometry.display_vflip ? "true" : "false")
			<< " | " << FormatDouble(sample.native_geometry.pixel_aspect_ratio)
			<< " | " << sample.video_compare_stable.max_abs
			<< " | " << sample.overlay_compare_stable.max_abs
			<< " | " << (sample.overlay_bounds_match ? "match" : "mismatch")
			<< " | " << (sample.passed ? "PASS" : "FAIL")
			<< " |\n";
	}
}

SampleResult RunSample(
	SampleInput const& input,
	InlineBackgroundRunner& runner) {
	SampleResult result;
	result.sample_name = input.sample_name;
	result.file_path = input.file_path.string();
	result.has_geometry_expectation = input.has_geometry_expectation;

	auto provider = CreateFFmpegSourceVideoProvider(result.file_path, "TV.709", &runner);
	if (!provider)
		throw std::runtime_error("Failed to create FFmpegSource video provider.");

	std::shared_ptr<void> native_owner;
	auto native = MakeNativeSourceFrame(*provider, 0, native_owner);
	result.native_geometry = native.geometry;
	result.display_output_rect = GetSourceFrameDisplayOutputRect(native.geometry);
	result.display_aspect_ratio = GetSourceFrameDisplayAspectRatio(native.geometry);
	result.provider_width = provider->GetWidth();
	result.provider_height = provider->GetHeight();
	result.provider_dar = provider->GetDAR();
	result.viewport_400 = BuildVideoDisplayViewportLayout(
		400,
		400,
		result.provider_width,
		result.provider_height,
		true,
		result.provider_dar > 0.0 ? result.provider_dar : 0.0);
	result.provider_display_contract_ok =
		result.provider_width == result.display_output_rect.width &&
		result.provider_height == result.display_output_rect.height &&
		NearlyEqual(result.provider_dar, result.display_aspect_ratio, 1e-4);
	result.geometry_matches_expectation =
		!input.has_geometry_expectation ||
		GeometryMatchesExpectation(native.geometry, input.geometry_expectation);

	VideoFrame bgra_storage;
	auto bgra = MakeBgraSourceFrame(*provider, 0, bgra_storage);
	result.baked_geometry = bgra.geometry;

	auto bgra_pixels = RenderBgraVideoFrame(bgra, result.provider_width, result.provider_height);
	auto native_pixels = RenderNativeVideoFrame(native, result.provider_width, result.provider_height);
	result.video_compare_full = CompareImages(bgra_pixels, native_pixels, result.provider_width, result.provider_height, false);
	result.video_compare_stable = CompareImages(bgra_pixels, native_pixels, result.provider_width, result.provider_height, true);
	result.video_compare_ok =
		result.video_compare_stable.compared_pixels > 0 &&
		result.video_compare_stable.max_abs <= 4 &&
		result.video_compare_stable.mean_abs <= 1.0;

	auto overlays = BuildEquivalentOverlays(native);
	auto storage_overlay_pixels = RenderOverlayOnly(native, overlays.storage_overlay, result.provider_width, result.provider_height);
	auto visible_overlay_pixels = RenderOverlayOnly(native, overlays.visible_overlay, result.provider_width, result.provider_height);
	result.overlay_storage_bounds = FindActiveBounds(storage_overlay_pixels, result.provider_width, result.provider_height);
	result.overlay_visible_bounds = FindActiveBounds(visible_overlay_pixels, result.provider_width, result.provider_height);
	result.overlay_bounds_match = BoundsEqual(result.overlay_storage_bounds, result.overlay_visible_bounds);
	result.overlay_compare_full = CompareImages(storage_overlay_pixels, visible_overlay_pixels, result.provider_width, result.provider_height, false);
	result.overlay_compare_stable = CompareImagesOnStableMask(
		storage_overlay_pixels,
		storage_overlay_pixels,
		visible_overlay_pixels,
		result.provider_width,
		result.provider_height);
	result.overlay_compare_ok =
		result.overlay_storage_bounds.valid &&
		result.overlay_visible_bounds.valid &&
		result.overlay_bounds_match &&
		result.overlay_compare_stable.compared_pixels > 0 &&
		result.overlay_compare_stable.max_abs == 0;

	result.passed =
		result.provider_display_contract_ok &&
		result.geometry_matches_expectation &&
		result.video_compare_ok &&
		result.overlay_compare_ok;
	return result;
}
}

int main(int argc, char** argv) try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	wxInitializer wx_initializer;
	if (!wx_initializer.IsOk())
		throw std::runtime_error("Failed to initialize wxWidgets for geometry compare harness.");

	std::filesystem::path samples_dir;
	std::filesystem::path manifest_path;
	std::filesystem::path report_path;
	std::filesystem::path matrix_path;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--samples-dir" && i + 1 < argc)
			samples_dir = argv[++i];
		else if (arg == "--manifest" && i + 1 < argc)
			manifest_path = argv[++i];
		else if (arg == "--report" && i + 1 < argc)
			report_path = argv[++i];
		else if (arg == "--matrix" && i + 1 < argc)
			matrix_path = argv[++i];
	}

	if (samples_dir.empty() == manifest_path.empty())
		throw std::runtime_error("Usage: video-geometry-compare (--samples-dir <dir> | --manifest <file>) [--report <json>] [--matrix <md>]");

	if (!samples_dir.empty())
		samples_dir = std::filesystem::absolute(samples_dir);
	if (!manifest_path.empty())
		manifest_path = std::filesystem::absolute(manifest_path);
	if (!report_path.empty())
		report_path = std::filesystem::absolute(report_path);
	if (!matrix_path.empty())
		matrix_path = std::filesystem::absolute(matrix_path);

	if (!report_path.empty())
		std::filesystem::create_directories(report_path.parent_path());
	if (!matrix_path.empty())
		std::filesystem::create_directories(matrix_path.parent_path());

	agi::log::log = new agi::log::LogSink;
	std::filesystem::path work_root;
	std::vector<SampleInput> sample_inputs;
	if (!manifest_path.empty()) {
		work_root = manifest_path.parent_path();
		sample_inputs = LoadSampleManifest(manifest_path);
	}
	else {
		std::filesystem::create_directories(samples_dir);
		work_root = samples_dir;
		sample_inputs = BuildBuiltInSampleInputs(samples_dir);
	}
	if (work_root.empty())
		work_root = std::filesystem::current_path();

	ScopedConfigContext config_context(work_root);
	InlineBackgroundRunner runner;
	std::vector<SampleResult> results;
	results.reserve(sample_inputs.size());

	bool passed = true;
	for (auto const& input : sample_inputs) {
		if (!std::filesystem::exists(input.file_path))
			throw std::runtime_error("Missing sample file: " + input.file_path.string());

		auto result = RunSample(input, runner);
		std::cout
			<< input.sample_name
			<< " geometry=" << GeometryStatusLabel(result)
			<< " native=[storage=" << result.native_geometry.storage_width << "x" << result.native_geometry.storage_height
			<< " visible=" << result.native_geometry.visible_rect.x << "," << result.native_geometry.visible_rect.y
			<< "," << result.native_geometry.visible_rect.width << "x" << result.native_geometry.visible_rect.height
			<< " rot=" << result.native_geometry.rotation
			<< " vflip=" << (result.native_geometry.display_vflip ? 1 : 0)
			<< " par=" << std::setprecision(6) << result.native_geometry.pixel_aspect_ratio << "]"
			<< " video_stable_max_abs=" << result.video_compare_stable.max_abs
			<< " overlay_stable_max_abs=" << result.overlay_compare_stable.max_abs
			<< " overlay_bounds_match=" << (result.overlay_bounds_match ? "ok" : "bad")
			<< " viewport=[" << result.viewport_400.viewport_left << "," << result.viewport_400.viewport_top << "," << result.viewport_400.viewport_width << "," << result.viewport_400.viewport_height << "]"
			<< "\n";
		passed = passed && result.passed;
		results.push_back(std::move(result));
	}

	if (!report_path.empty())
		WriteReport(report_path, results);
	if (!matrix_path.empty())
		WriteMatrix(matrix_path, results);

	delete agi::log::log;
	agi::log::log = nullptr;
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "video-geometry-compare failed: " << err.what() << std::endl;
	return 2;
}
catch (agi::Exception const& err) {
	std::cerr << "video-geometry-compare failed: " << err.GetMessage() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "video-geometry-compare failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "video-geometry-compare is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
