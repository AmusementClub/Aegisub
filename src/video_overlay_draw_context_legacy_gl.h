// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "video_overlay_draw_context.h"

class OpenGLText;
class OpenGLWrapper;

class LegacyVideoOverlayDrawContext final : public VideoOverlayDrawContext {
	OpenGLWrapper &gl;
	OpenGLText &text;

public:
	LegacyVideoOverlayDrawContext(OpenGLWrapper &gl, OpenGLText &text);

	void SetLineColour(wxColour const& colour, float alpha = 1.0f, int width = 1) override;
	void SetFillColour(wxColour const& colour, float alpha = 1.0f) override;
	void SetInvert() override;
	void ClearInvert() override;

	void DrawLine(Vector2D p1, Vector2D p2) override;
	void DrawLines(size_t dim, float const *lines, size_t n) override;
	void DrawLineStrip(Vector2D const *points, size_t n) override;
	void DrawRectangle(Vector2D p1, Vector2D p2) override;
	void DrawPolygon(Vector2D const *points, size_t n) override;
	void DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) override;
	void DrawCircle(Vector2D center, float radius) override;
	void DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) override;

	wxSize MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override;
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override;
};
