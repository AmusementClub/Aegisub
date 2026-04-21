// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "vector2d.h"

#include <cstddef>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/gdicmn.h>

struct VideoOverlayTextStyle {
	std::string face;
	int size = 12;
	bool bold = false;
	bool italic = false;
	wxColour colour = *wxWHITE;
	bool outline = true;
};

class VideoOverlayDrawContext {
public:
	virtual ~VideoOverlayDrawContext() = default;

	virtual void SetLineColour(wxColour const& colour, float alpha = 1.0f, int width = 1) = 0;
	virtual void SetFillColour(wxColour const& colour, float alpha = 1.0f) = 0;
	virtual void SetInvert() = 0;
	virtual void ClearInvert() = 0;

	virtual void DrawLine(Vector2D p1, Vector2D p2) = 0;
	virtual void DrawLines(size_t dim, float const *lines, size_t n) = 0;
	virtual void DrawLineStrip(Vector2D const *points, size_t n) = 0;
	virtual void DrawRectangle(Vector2D p1, Vector2D p2) = 0;
	virtual void DrawPolygon(Vector2D const *points, size_t n) = 0;
	virtual void DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) = 0;
	virtual void DrawCircle(Vector2D center, float radius) = 0;
	virtual void DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) = 0;

	virtual wxSize MeasureText(std::string const& text, VideoOverlayTextStyle const& style) = 0;
	virtual void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) = 0;
};
