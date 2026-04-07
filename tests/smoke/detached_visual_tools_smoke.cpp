#include "../../src/ivideo_renderer.h"
#include "../../src/gl_wrap.h"
#include "../../src/source_frame.h"
#include "../../src/video_display_layout.h"
#include "../../src/video_frame.h"
#include "../../src/video_renderer_error.h"
#include "../../src/video_renderer_opengl.h"
#include "../../src/visual_feature.h"

#ifdef WITH_LIBPLACEBO
#include "../../src/video_renderer_placebo_gl.h"
#endif

#ifdef _WIN32

#include <libaegisub/exception.h>
#include <libaegisub/log.h>

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
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <wx/colour.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// vector2d.cpp references this formatting helper, but this smoke does not
// exercise Vector2D's text formatting paths.
std::string float_to_string(double val) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.3f", val);
	std::string result(buffer);
	auto pos = result.find_last_not_of('0');
	if (pos != std::string::npos) {
		if (result[pos] != '.')
			++pos;
		result.erase(pos);
	}
	return result;
}

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
using WglCreateContextAttribsArbProc = HGLRC(WINAPI*)(HDC, HGLRC, int const*);

void SehTranslator(unsigned int code, EXCEPTION_POINTERS*) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "SEH exception 0x%08X", code);
	throw std::runtime_error(buffer);
}

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubDetachedVisualToolsSmokeWindow";
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
			throw std::runtime_error("RegisterClassA failed for detached visual tools smoke window.");

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
			"Aegisub detached visual tools smoke",
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
			throw std::runtime_error("CreateWindowExA failed for detached visual tools smoke window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for detached visual tools smoke window.");

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
			throw std::runtime_error("ChoosePixelFormat failed for detached visual tools smoke window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for detached visual tools smoke window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for detached visual tools smoke window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for detached visual tools smoke window.");

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
					throw std::runtime_error("wglMakeCurrent failed for modern detached visual tools smoke window.");
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
			throw std::runtime_error("wglMakeCurrent failed for detached visual tools smoke window.");
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

struct BgraScenario {
	VideoFrame storage;
	SourceFrame frame;
};

struct OverlayScenario {
	std::string name;
	int canvas_width = 0;
	int canvas_height = 0;
	int source_width = 0;
	int source_height = 0;
	double target_display_aspect_ratio = 1.0;
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	Vector2D mouse_pos;
	Vector2D script_res;
	Vector2D feature_script_pos;
};

struct PixelColorExpectation {
	std::string label;
	int x = 0;
	int y = 0;
	int min_r = 0;
	int min_g = 0;
	int min_b = 0;
	int max_r = 255;
	int max_g = 255;
	int max_b = 255;
};

struct PixelChangeExpectation {
	std::string label;
	int x = 0;
	int y = 0;
	int min_delta = 32;
};

void FillBgraScenario(BgraScenario& scenario, int width, int height) {
	scenario.storage.width = static_cast<std::size_t>(width);
	scenario.storage.height = static_cast<std::size_t>(height);
	scenario.storage.pitch = static_cast<std::size_t>(width) * 4;
	scenario.storage.flipped = false;
	scenario.storage.data.resize(scenario.storage.pitch * scenario.storage.height);

	for (int y = 0; y < height; ++y) {
		auto* row = scenario.storage.data.data() + static_cast<std::size_t>(y) * scenario.storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = static_cast<unsigned char>((24 + x * 3 + y) & 0xFF);
			pixel[1] = static_cast<unsigned char>((40 + y * 4) & 0xFF);
			pixel[2] = static_cast<unsigned char>((72 + x * 2) & 0xFF);
			pixel[3] = 255;
		}
	}

	scenario.frame = MakeSourceFrameView(
		scenario.storage,
		SourceFrameColorMetadata { "RGB", "BT.709", "BT.1886", SourceFrameColorRange::Full });
}

BgraScenario MakeBgraScenario(int width, int height) {
	BgraScenario scenario;
	FillBgraScenario(scenario, width, height);
	return scenario;
}

void SetupOverlayProjection(int width, int height) {
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, width, height, 0.0, -1000.0, 1000.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_COLOR_LOGIC_OP);
}

void DrawVisualToolCross(OpenGLWrapper& gl, int canvas_width, int canvas_height, Vector2D mouse_pos) {
	gl.SetInvert();
	gl.SetLineColour(wxColour(255, 255, 255), 1.0f, 1);
	float lines[] = {
		0.0f, mouse_pos.Y(),
		static_cast<float>(canvas_width), mouse_pos.Y(),
		mouse_pos.X(), 0.0f,
		mouse_pos.X(), static_cast<float>(canvas_height)
	};
	gl.DrawLines(2, lines, 4);
	gl.ClearInvert();
}

void DrawVisualToolFeatureMarker(OpenGLWrapper const& gl, Vector2D feature_position) {
	VisualDraggableFeature feature;
	feature.type = DRAG_BIG_SQUARE;
	feature.pos = feature_position;
	feature.Draw(gl);
}

bool PixelMatchesColor(
	std::vector<unsigned char> const& pixels,
	int width,
	PixelColorExpectation const& expectation) {
	auto const sample_index = static_cast<std::size_t>(expectation.y * width + expectation.x) * 4;
	int const sample_r = pixels[sample_index + 0];
	int const sample_g = pixels[sample_index + 1];
	int const sample_b = pixels[sample_index + 2];
	return sample_r >= expectation.min_r && sample_r <= expectation.max_r
		&& sample_g >= expectation.min_g && sample_g <= expectation.max_g
		&& sample_b >= expectation.min_b && sample_b <= expectation.max_b;
}

bool PixelChangedEnough(
	std::vector<unsigned char> const& before,
	std::vector<unsigned char> const& after,
	int width,
	int height,
	PixelChangeExpectation const& expectation) {
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int const x = expectation.x + dx;
			int const y = expectation.y + dy;
			if (x < 0 || y < 0 || x >= width || y >= height)
				continue;

			auto const sample_index = static_cast<std::size_t>(y * width + x) * 4;
			int const delta_r = std::abs(static_cast<int>(before[sample_index + 0]) - static_cast<int>(after[sample_index + 0]));
			int const delta_g = std::abs(static_cast<int>(before[sample_index + 1]) - static_cast<int>(after[sample_index + 1]));
			int const delta_b = std::abs(static_cast<int>(before[sample_index + 2]) - static_cast<int>(after[sample_index + 2]));
			if (std::max({ delta_r, delta_g, delta_b }) >= expectation.min_delta)
				return true;
		}
	}
	return false;
}

Vector2D ComputeFeaturePosition(
	VideoDisplayViewportLayout const& layout,
	Vector2D script_res,
	Vector2D script_position) {
	Vector2D video_pos(layout.viewport_left, layout.viewport_top);
	Vector2D video_res(layout.viewport_width, layout.viewport_height);
	return video_pos + script_position * video_res / script_res;
}

std::vector<PixelChangeExpectation> BuildChangeExpectations(
	OverlayScenario const& scenario) {
	int const mouse_x = static_cast<int>(std::lround(scenario.mouse_pos.X()));
	int const mouse_y = static_cast<int>(std::lround(scenario.mouse_pos.Y()));
	return {
		{ "left_cross", 4, mouse_y, 32 },
		{ "right_cross", scenario.canvas_width - 5, mouse_y, 32 },
		{ "top_cross", mouse_x, 4, 32 },
		{ "bottom_cross", mouse_x, scenario.canvas_height - 5, 32 }
	};
}

std::vector<PixelColorExpectation> BuildColorExpectations(
	OverlayScenario const& scenario,
	Vector2D feature_position) {
	int const feature_x = static_cast<int>(std::lround(feature_position.X()));
	int const feature_y = static_cast<int>(std::lround(feature_position.Y()));
	return {
		{ "feature_marker", feature_x, feature_y, 200, 0, 0, 255, 80, 80 }
	};
}

void PrintFailure(
	std::string const& renderer_name,
	OverlayScenario const& scenario,
	PixelColorExpectation const& expectation,
	std::vector<unsigned char> const& pixels) {
	auto const sample_index = static_cast<std::size_t>(expectation.y * scenario.canvas_width + expectation.x) * 4;
	std::cout
		<< renderer_name << "/" << scenario.name << " failed at " << expectation.label
		<< " (" << expectation.x << ", " << expectation.y << ")"
		<< " rgba=("
		<< static_cast<int>(pixels[sample_index + 0]) << ", "
		<< static_cast<int>(pixels[sample_index + 1]) << ", "
		<< static_cast<int>(pixels[sample_index + 2]) << ", "
		<< static_cast<int>(pixels[sample_index + 3]) << ")\n";
}

void PrintChangeFailure(
	std::string const& renderer_name,
	OverlayScenario const& scenario,
	PixelChangeExpectation const& expectation,
	std::vector<unsigned char> const& before,
	std::vector<unsigned char> const& after) {
	auto const sample_index = static_cast<std::size_t>(expectation.y * scenario.canvas_width + expectation.x) * 4;
	std::cout
		<< renderer_name << "/" << scenario.name << " failed at " << expectation.label
		<< " (" << expectation.x << ", " << expectation.y << ")"
		<< " before=("
		<< static_cast<int>(before[sample_index + 0]) << ", "
		<< static_cast<int>(before[sample_index + 1]) << ", "
		<< static_cast<int>(before[sample_index + 2]) << ", "
		<< static_cast<int>(before[sample_index + 3]) << ")"
		<< " after=("
		<< static_cast<int>(after[sample_index + 0]) << ", "
		<< static_cast<int>(after[sample_index + 1]) << ", "
		<< static_cast<int>(after[sample_index + 2]) << ", "
		<< static_cast<int>(after[sample_index + 3]) << ")"
		<< " min_delta=" << expectation.min_delta
		<< "\n";
}

template<class RendererFactory>
bool ValidateOverlayScenario(
	RendererFactory const& factory,
	char const* renderer_name,
	OverlayScenario const& scenario) {
	HiddenGLWindow window(scenario.canvas_width, scenario.canvas_height);
	window.MakeCurrent();
	auto renderer = factory();

	auto frame = MakeBgraScenario(scenario.source_width, scenario.source_height);
	auto base_viewport = BuildVideoDisplayViewportLayout(
		scenario.canvas_width,
		scenario.canvas_height,
		scenario.canvas_width,
		scenario.canvas_height,
		true,
		scenario.target_display_aspect_ratio);
	auto viewport = BuildVideoDisplayContentLayout(
		base_viewport,
		scenario.canvas_height,
		scenario.transform.pan_x != 0.0 || scenario.transform.pan_y != 0.0,
		scenario.transform);
	Vector2D feature_position = ComputeFeaturePosition(viewport, scenario.script_res, scenario.feature_script_pos);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	renderer->UploadFrame(frame.frame);
	renderer->UploadOverlay(nullptr);
	renderer->Render(
		{ viewport.viewport_left, viewport.viewport_bottom, viewport.viewport_width, viewport.viewport_height },
		scenario.canvas_width,
		scenario.canvas_height);
	auto before_overlay = window.ReadBackRgbaTopLeft();

	SetupOverlayProjection(scenario.canvas_width, scenario.canvas_height);
	OpenGLWrapper gl;
	DrawVisualToolCross(gl, scenario.canvas_width, scenario.canvas_height, scenario.mouse_pos);
	gl.SetLineColour(wxColour(255, 32, 32), 1.0f, 3);
	gl.SetFillColour(wxColour(255, 32, 32), 1.0f);
	DrawVisualToolFeatureMarker(gl, feature_position);

	auto pixels = window.ReadBackRgbaTopLeft();
	bool passed = true;
	for (auto const& expectation : BuildChangeExpectations(scenario)) {
		if (!PixelChangedEnough(before_overlay, pixels, scenario.canvas_width, scenario.canvas_height, expectation)) {
			PrintChangeFailure(renderer_name, scenario, expectation, before_overlay, pixels);
			passed = false;
		}
	}
	for (auto const& expectation : BuildColorExpectations(scenario, feature_position)) {
		if (!PixelMatchesColor(pixels, scenario.canvas_width, expectation)) {
			PrintFailure(renderer_name, scenario, expectation, pixels);
			passed = false;
		}
	}
	if (passed)
		std::cout << renderer_name << "/" << scenario.name << " overlay=ok\n";
	renderer->Reset();
	return passed;
}

template<class RendererFactory>
bool ValidateRenderer(char const* renderer_name, RendererFactory&& factory, std::vector<OverlayScenario> const& scenarios) {
	bool passed = true;
	for (auto const& scenario : scenarios) {
		passed = ValidateOverlayScenario(factory, renderer_name, scenario) && passed;
	}
	return passed;
}

std::vector<OverlayScenario> BuildScenarios() {
	return {
		{
			"wide_panned_down",
			240,
			160,
			160,
			90,
			16.0 / 9.0,
			{ 1.0, 0.2, 0.25 },
			Vector2D(70, 36),
			Vector2D(192, 108),
			Vector2D(128, 64)
		},
		{
			"pillarboxed_panned_right",
			200,
			200,
			120,
			160,
			3.0 / 4.0,
			{ 1.0, 0.35, 0.0 },
			Vector2D(124, 90),
			Vector2D(160, 120),
			Vector2D(64, 72)
		}
	};
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	agi::log::log = new agi::log::LogSink;

	auto scenarios = BuildScenarios();
	bool passed = true;

	passed = ValidateRenderer(
		"opengl",
		[] { return std::make_unique<OpenGLVideoRenderer>(true, false, true); },
		scenarios) && passed;

#ifdef WITH_LIBPLACEBO
	passed = ValidateRenderer(
		"placebo",
		[] { return std::make_unique<PlaceboRendererGL>(); },
		scenarios) && passed;
#endif

	delete agi::log::log;
	agi::log::log = nullptr;
	return passed ? 0 : 1;
}
catch (std::exception const& err) {
	std::cerr << "detached-visual-tools-smoke failed: " << err.what() << std::endl;
	delete agi::log::log;
	agi::log::log = nullptr;
	return 1;
}
catch (agi::Exception const& err) {
	std::cerr << "detached-visual-tools-smoke failed: " << err.GetMessage() << std::endl;
	delete agi::log::log;
	agi::log::log = nullptr;
	return 1;
}
catch (...) {
	std::cerr << "detached-visual-tools-smoke failed: unknown exception" << std::endl;
	delete agi::log::log;
	agi::log::log = nullptr;
	return 1;
}

#else

#include <iostream>

int main() {
	std::cout << "detached-visual-tools-smoke is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
