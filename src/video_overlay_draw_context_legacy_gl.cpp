// Copyright (c) 2026
// All rights reserved.

#include "video_overlay_draw_context_legacy_gl.h"

#include "gl_text.h"
#include "gl_wrap.h"

#include <libaegisub/color.h>

LegacyVideoOverlayDrawContext::LegacyVideoOverlayDrawContext(OpenGLWrapper &gl, OpenGLText &text)
: gl(gl)
, text(text) {
}

void LegacyVideoOverlayDrawContext::SetLineColour(wxColour const& colour, float alpha, int width) {
	gl.SetLineColour(colour, alpha, width);
}

void LegacyVideoOverlayDrawContext::SetFillColour(wxColour const& colour, float alpha) {
	gl.SetFillColour(colour, alpha);
}

void LegacyVideoOverlayDrawContext::SetInvert() {
	gl.SetInvert();
}

void LegacyVideoOverlayDrawContext::ClearInvert() {
	gl.ClearInvert();
}

void LegacyVideoOverlayDrawContext::DrawLine(Vector2D p1, Vector2D p2) {
	gl.DrawLine(p1, p2);
}

void LegacyVideoOverlayDrawContext::DrawLines(size_t dim, float const *lines, size_t n) {
	gl.DrawLines(dim, lines, n);
}

void LegacyVideoOverlayDrawContext::DrawLineStrip(Vector2D const *points, size_t n) {
	if (!points || n < 2)
		return;

	std::vector<float> coords;
	coords.reserve(n * 2);
	for (size_t i = 0; i < n; ++i) {
		coords.push_back(points[i].X());
		coords.push_back(points[i].Y());
	}
	gl.DrawLineStrip(2, coords);
}

void LegacyVideoOverlayDrawContext::DrawRectangle(Vector2D p1, Vector2D p2) {
	gl.DrawRectangle(p1, p2);
}

void LegacyVideoOverlayDrawContext::DrawPolygon(Vector2D const *points, size_t n) {
	gl.DrawPolygon(points, n);
}

void LegacyVideoOverlayDrawContext::DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) {
	std::vector<int> mutable_start = start;
	std::vector<int> mutable_count = count;
	gl.DrawMultiPolygon(points, mutable_start, mutable_count, video_pos, video_size, invert);
}

void LegacyVideoOverlayDrawContext::DrawCircle(Vector2D center, float radius) {
	gl.DrawCircle(center, radius);
}

void LegacyVideoOverlayDrawContext::DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) {
	gl.DrawTriangle(p1, p2, p3);
}

wxSize LegacyVideoOverlayDrawContext::MeasureText(std::string const& value, VideoOverlayTextStyle const& style) {
	text.SetFont(style.face, style.size, style.bold, style.italic);
	int width = 0;
	int height = 0;
	text.GetExtent(value, width, height);
	return wxSize(width, height);
}

void LegacyVideoOverlayDrawContext::DrawText(std::string const& value, int x, int y, VideoOverlayTextStyle const& style) {
	text.SetFont(style.face, style.size, style.bold, style.italic);
	text.SetColour(agi::Color(style.colour.Red(), style.colour.Green(), style.colour.Blue(), style.colour.Alpha()));
	text.Print(value, x, y);
}
