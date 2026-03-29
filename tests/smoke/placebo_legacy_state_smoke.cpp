#ifdef WITH_LIBPLACEBO

#ifdef _WIN32

#include "../../src/source_frame.h"
#include "../../src/video_frame.h"
#include "../../src/video_renderer_placebo_gl.h"

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
		return "AegisubPlaceboLegacyStateSmokeWindow";
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
			throw std::runtime_error("RegisterClassA failed for placebo legacy-state smoke window.");

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
			"Aegisub placebo legacy-state smoke",
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
			throw std::runtime_error("CreateWindowExA failed for placebo legacy-state smoke window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for placebo legacy-state smoke window.");

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
			throw std::runtime_error("ChoosePixelFormat failed for placebo legacy-state smoke window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for placebo legacy-state smoke window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for placebo legacy-state smoke window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for placebo legacy-state smoke window.");

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
					throw std::runtime_error("wglMakeCurrent failed for modern placebo legacy-state smoke window.");
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
			throw std::runtime_error("wglMakeCurrent failed for placebo legacy-state smoke window.");
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

void FillBgraScenario(BgraScenario& scenario, int width, int height, int frame_index) {
	scenario.storage.width = static_cast<std::size_t>(width);
	scenario.storage.height = static_cast<std::size_t>(height);
	scenario.storage.pitch = static_cast<std::size_t>(width) * 4;
	scenario.storage.flipped = false;
	scenario.storage.data.resize(scenario.storage.pitch * scenario.storage.height);

	int const x_bias = frame_index * 7;
	int const y_bias = frame_index * 11;
	for (int y = 0; y < height; ++y) {
		auto* row = scenario.storage.data.data() + static_cast<std::size_t>(y) * scenario.storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = static_cast<unsigned char>((32 + x + x_bias) & 0xFF);
			pixel[1] = static_cast<unsigned char>((64 + y + y_bias) & 0xFF);
			pixel[2] = static_cast<unsigned char>((96 + frame_index * 13) & 0xFF);
			pixel[3] = 255;
		}
	}
}

void RefreshBgraScenarioFrameView(BgraScenario& scenario) {
	scenario.frame = MakeSourceFrameView(
		scenario.storage,
		SourceFrameColorMetadata { "RGB", "BT.709", "BT.1886", SourceFrameColorRange::Full });
}

BgraScenario MakeBgraScenario(int width, int height, int frame_index = 0) {
	BgraScenario scenario;
	FillBgraScenario(scenario, width, height, frame_index);
	RefreshBgraScenarioFrameView(scenario);
	return scenario;
}

struct ClientArrayQuad {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
};

struct Point2 {
	float x = 0.0f;
	float y = 0.0f;
};

struct LegacyPrimitiveLayout {
	ClientArrayQuad marker;
	Point2 solid_line_start;
	Point2 solid_line_end;
	Point2 dashed_line_start;
	Point2 dashed_line_end;
	Point2 triangle_a;
	Point2 triangle_b;
	Point2 triangle_c;
};

Point2 MakePoint(float x, float y) {
	return { x, y };
}

Point2 Interpolate(Point2 p1, Point2 p2, float t) {
	return {
		t * p1.x + (1.0f - t) * p2.x,
		t * p1.y + (1.0f - t) * p2.y
	};
}

float Distance(Point2 p1, Point2 p2) {
	float dx = p2.x - p1.x;
	float dy = p2.y - p1.y;
	return std::sqrt(dx * dx + dy * dy);
}

void SetupLegacy2DProjection(int canvas_width, int canvas_height) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, canvas_width, canvas_height, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

LegacyPrimitiveLayout MakePrimitiveLayout(int frame_index) {
	LegacyPrimitiveLayout layout;
	layout.marker = {
		8 + frame_index * 16,
		10 + (frame_index % 3) * 8,
		18 + frame_index * 16,
		20 + (frame_index % 3) * 8
	};
	layout.solid_line_start = MakePoint(10 + frame_index * 16.0f, 58.0f);
	layout.solid_line_end = MakePoint(30 + frame_index * 16.0f, 58.0f);
	layout.dashed_line_start = MakePoint(10 + frame_index * 16.0f, 74.0f);
	layout.dashed_line_end = MakePoint(42 + frame_index * 16.0f, 74.0f);
	layout.triangle_a = MakePoint(14 + frame_index * 16.0f, 30.0f);
	layout.triangle_b = MakePoint(30 + frame_index * 16.0f, 38.0f);
	layout.triangle_c = MakePoint(18 + frame_index * 16.0f, 48.0f);
	return layout;
}

void DrawLegacyClientArrayQuad(int canvas_width, int canvas_height, ClientArrayQuad const& quad) {
	SetupLegacy2DProjection(canvas_width, canvas_height);
	glDisable(GL_BLEND);
	glColor4ub(255, 0, 0, 255);

	GLfloat vertices[] = {
		static_cast<GLfloat>(quad.x0), static_cast<GLfloat>(quad.y0),
		static_cast<GLfloat>(quad.x1), static_cast<GLfloat>(quad.y0),
		static_cast<GLfloat>(quad.x1), static_cast<GLfloat>(quad.y1),
		static_cast<GLfloat>(quad.x0), static_cast<GLfloat>(quad.y1)
	};
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void DrawLegacyLine(Point2 p1, Point2 p2, unsigned char r, unsigned char g, unsigned char b, float width) {
	GLfloat vertices[] = {
		p1.x, p1.y,
		p2.x, p2.y
	};
	glDisable(GL_BLEND);
	glColor4ub(r, g, b, 255);
	glLineWidth(width);
	glDisable(GL_LINE_SMOOTH);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glDrawArrays(GL_LINES, 0, 2);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void DrawLegacyDashedLine(Point2 p1, Point2 p2, float dash_len, unsigned char r, unsigned char g, unsigned char b, float width) {
	float step = dash_len / Distance(p1, p2);
	for (float t = 0.0f; t < 1.0f; t += 2.0f * step)
		DrawLegacyLine(Interpolate(p1, p2, t), Interpolate(p1, p2, std::min(1.0f, t + step)), r, g, b, width);
}

void DrawLegacyTriangle(Point2 p1, Point2 p2, Point2 p3, unsigned char r, unsigned char g, unsigned char b) {
	GLfloat vertices[] = {
		p1.x, p1.y,
		p2.x, p2.y,
		p3.x, p3.y
	};
	glDisable(GL_BLEND);
	glColor4ub(r, g, b, 255);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void DrawVisualToolLikePrimitives(int canvas_width, int canvas_height, LegacyPrimitiveLayout const& layout) {
	DrawLegacyClientArrayQuad(canvas_width, canvas_height, layout.marker);

	SetupLegacy2DProjection(canvas_width, canvas_height);
	DrawLegacyLine(layout.solid_line_start, layout.solid_line_end, 0, 255, 0, 4.0f);
	DrawLegacyDashedLine(layout.dashed_line_start, layout.dashed_line_end, 6.0f, 0, 96, 255, 4.0f);
	DrawLegacyTriangle(layout.triangle_a, layout.triangle_b, layout.triangle_c, 255, 224, 0);
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

Point2 TriangleCentroid(LegacyPrimitiveLayout const& layout) {
	return {
		(layout.triangle_a.x + layout.triangle_b.x + layout.triangle_c.x) / 3.0f,
		(layout.triangle_a.y + layout.triangle_b.y + layout.triangle_c.y) / 3.0f
	};
}

bool ValidateLegacyClientArraysDuringPlaybackLikeSequence() {
	constexpr int width = 192;
	constexpr int height = 96;
	constexpr int frame_count = 8;

	HiddenGLWindow window(width, height);
	window.MakeCurrent();
	PlaceboRendererGL renderer;
	BgraScenario bgra = MakeBgraScenario(width, height, 0);
	LegacyPrimitiveLayout previous_layout = { };
	bool have_previous_layout = false;

	for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
		FillBgraScenario(bgra, width, height, frame_index);
		RefreshBgraScenarioFrameView(bgra);

		auto const current_layout = MakePrimitiveLayout(frame_index);

		glViewport(0, 0, width, height);
		renderer.UploadFrame(bgra.frame);
		renderer.UploadOverlay(nullptr);
		renderer.Render({ 0, 0, width, height }, width, height);

		GLint current_program = -1;
		GLint array_buffer = -1;
		GLint element_array_buffer = -1;
		GLint active_texture = -1;
		glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_array_buffer);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);

		GLint vertex_array = 0;
#ifdef GL_VERTEX_ARRAY_BINDING
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
#endif

		bool const bindings_restored =
			current_program == 0
			&& array_buffer == 0
			&& element_array_buffer == 0
			&& active_texture == GL_TEXTURE0
			&& vertex_array == 0;
		if (!bindings_restored) {
			std::cout
				<< "frame=" << frame_index
				<< " current_program=" << current_program
				<< " array_buffer=" << array_buffer
				<< " element_array_buffer=" << element_array_buffer
				<< " vertex_array=" << vertex_array
				<< " active_texture=0x" << std::hex << active_texture << std::dec
				<< "\n";
			return false;
		}

		DrawVisualToolLikePrimitives(width, height, current_layout);
		auto pixels = window.ReadBackRgbaTopLeft();
		int const marker_center_x = (current_layout.marker.x0 + current_layout.marker.x1) / 2;
		int const marker_center_y = (current_layout.marker.y0 + current_layout.marker.y1) / 2;
		int const solid_line_x = static_cast<int>((current_layout.solid_line_start.x + current_layout.solid_line_end.x) / 2.0f);
		int const solid_line_y = static_cast<int>(current_layout.solid_line_start.y);
		int const dashed_line_x = static_cast<int>(current_layout.dashed_line_start.x + 3.0f);
		int const dashed_line_y = static_cast<int>(current_layout.dashed_line_start.y);
		auto const triangle_centroid = TriangleCentroid(current_layout);
		int const triangle_x = static_cast<int>(triangle_centroid.x);
		int const triangle_y = static_cast<int>(triangle_centroid.y);

		bool const current_marker_visible = PixelMatchesColor(pixels, width, marker_center_x, marker_center_y, 200, 0, 0, 255, 32, 32);
		bool const current_line_visible = PixelMatchesColor(pixels, width, solid_line_x, solid_line_y, 0, 160, 0, 80, 255, 80);
		bool const current_dashed_visible = PixelMatchesColor(pixels, width, dashed_line_x, dashed_line_y, 0, 32, 160, 80, 160, 255);
		bool const current_triangle_visible = PixelMatchesColor(pixels, width, triangle_x, triangle_y, 200, 160, 0, 255, 255, 96);

		bool previous_marker_cleared = true;
		bool previous_line_cleared = true;
		bool previous_dashed_cleared = true;
		bool previous_triangle_cleared = true;
		if (have_previous_layout) {
			int const previous_marker_x = (previous_layout.marker.x0 + previous_layout.marker.x1) / 2;
			int const previous_marker_y = (previous_layout.marker.y0 + previous_layout.marker.y1) / 2;
			int const previous_line_x = static_cast<int>((previous_layout.solid_line_start.x + previous_layout.solid_line_end.x) / 2.0f);
			int const previous_line_y = static_cast<int>(previous_layout.solid_line_start.y);
			int const previous_dashed_x = static_cast<int>(previous_layout.dashed_line_start.x + 3.0f);
			int const previous_dashed_y = static_cast<int>(previous_layout.dashed_line_start.y);
			auto const previous_triangle_centroid = TriangleCentroid(previous_layout);
			int const previous_triangle_x = static_cast<int>(previous_triangle_centroid.x);
			int const previous_triangle_y = static_cast<int>(previous_triangle_centroid.y);

			previous_marker_cleared = !PixelMatchesColor(pixels, width, previous_marker_x, previous_marker_y, 200, 0, 0, 255, 32, 32);
			previous_line_cleared = !PixelMatchesColor(pixels, width, previous_line_x, previous_line_y, 0, 160, 0, 80, 255, 80);
			previous_dashed_cleared = !PixelMatchesColor(pixels, width, previous_dashed_x, previous_dashed_y, 0, 32, 160, 80, 160, 255);
			previous_triangle_cleared = !PixelMatchesColor(pixels, width, previous_triangle_x, previous_triangle_y, 200, 160, 0, 255, 255, 96);
		}

		std::cout
			<< "frame=" << frame_index
			<< " marker=" << current_marker_visible
			<< " line=" << current_line_visible
			<< " dashed=" << current_dashed_visible
			<< " triangle=" << current_triangle_visible
			<< " prev_marker=" << previous_marker_cleared
			<< " prev_line=" << previous_line_cleared
			<< " prev_dashed=" << previous_dashed_cleared
			<< " prev_triangle=" << previous_triangle_cleared
			<< "\n";

		if (!current_marker_visible
			|| !current_line_visible
			|| !current_dashed_visible
			|| !current_triangle_visible
			|| !previous_marker_cleared
			|| !previous_line_cleared
			|| !previous_dashed_cleared
			|| !previous_triangle_cleared) {
			return false;
		}

		previous_layout = current_layout;
		have_previous_layout = true;
	}

	return true;
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	agi::log::log = new agi::log::LogSink;

	bool const passed = ValidateLegacyClientArraysDuringPlaybackLikeSequence();
	delete agi::log::log;
	agi::log::log = nullptr;
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "placebo-legacy-state-smoke failed: " << err.what() << std::endl;
	return 2;
}
catch (agi::Exception const& err) {
	std::cerr << "placebo-legacy-state-smoke failed: " << err.GetMessage() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "placebo-legacy-state-smoke failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "placebo-legacy-state-smoke is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif

#else

int main() {
	return 0;
}

#endif
