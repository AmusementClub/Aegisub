// Copyright (c) 2026
// All rights reserved.

#include "video_overlay_draw_context_skia.h"

#include "skia_runtime/skia_text_layout_cache.h"

#ifdef WITH_SKIA
#include <include/core/SkBlendMode.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRect.h>
#endif

#include <algorithm>

namespace {
#ifdef WITH_SKIA
SkColor ToSkColor(wxColour const& colour, float alpha) {
	return SkColorSetARGB(
		static_cast<U8CPU>(std::clamp(alpha, 0.0f, 1.0f) * colour.Alpha()),
		colour.Red(),
		colour.Green(),
		colour.Blue());
}
#endif
}

SkiaVideoOverlayDrawContext::SkiaVideoOverlayDrawContext(SkCanvas &canvas, SkCanvas *invert_canvas, SkiaTextLayoutCache &text_cache, float device_scale)
: canvas(canvas)
, invert_canvas(invert_canvas)
, text_cache(text_cache)
, device_scale(std::max(1.0f, device_scale)) {
}

void SkiaVideoOverlayDrawContext::SetLineColour(wxColour const& colour, float alpha, int width) {
	line_colour = colour;
	line_alpha = alpha;
	line_width = width;
}

void SkiaVideoOverlayDrawContext::SetFillColour(wxColour const& colour, float alpha) {
	fill_colour = colour;
	fill_alpha = alpha;
}

void SkiaVideoOverlayDrawContext::SetInvert() {
	invert = true;
}

void SkiaVideoOverlayDrawContext::ClearInvert() {
	invert = false;
}

#ifdef WITH_SKIA
SkCanvas &SkiaVideoOverlayDrawContext::GetTargetCanvas() const {
	return (invert && invert_canvas) ? *invert_canvas : canvas;
}

SkPaint SkiaVideoOverlayDrawContext::MakeStrokePaint() const {
	SkPaint paint;
	paint.setAntiAlias(!(invert && invert_canvas));
	paint.setStyle(SkPaint::kStroke_Style);
	paint.setStrokeWidth(std::max(1.0f, static_cast<float>(line_width)) / device_scale);
	if (invert && invert_canvas) {
		paint.setColor(SK_ColorWHITE);
	}
	else if (invert) {
		paint.setBlendMode(SkBlendMode::kDifference);
		paint.setColor(SK_ColorWHITE);
	}
	else {
		paint.setColor(ToSkColor(line_colour, line_alpha));
	}
	return paint;
}

SkPaint SkiaVideoOverlayDrawContext::MakeFillPaint() const {
	SkPaint paint;
	paint.setAntiAlias(!(invert && invert_canvas));
	paint.setStyle(SkPaint::kFill_Style);
	if (invert && invert_canvas) {
		paint.setColor(SK_ColorWHITE);
	}
	else if (invert) {
		paint.setBlendMode(SkBlendMode::kDifference);
		paint.setColor(SK_ColorWHITE);
	}
	else {
		paint.setColor(ToSkColor(fill_colour, fill_alpha));
	}
	return paint;
}
#endif

void SkiaVideoOverlayDrawContext::DrawLine(Vector2D p1, Vector2D p2) {
#ifdef WITH_SKIA
	GetTargetCanvas().drawLine(p1.X(), p1.Y(), p2.X(), p2.Y(), MakeStrokePaint());
#else
	(void)p1;
	(void)p2;
#endif
}

void SkiaVideoOverlayDrawContext::DrawLines(size_t dim, float const *lines, size_t n) {
#ifdef WITH_SKIA
	if (dim != 2 || !lines || n < 2)
		return;

	auto const paint = MakeStrokePaint();
	SkCanvas &target = GetTargetCanvas();
	for (size_t i = 0; i + 1 < n; i += 2) {
		size_t const offset = i * dim;
		target.drawLine(lines[offset], lines[offset + 1], lines[offset + 2], lines[offset + 3], paint);
	}
#else
	(void)dim;
	(void)lines;
	(void)n;
#endif
}

void SkiaVideoOverlayDrawContext::DrawLineStrip(Vector2D const *points, size_t n) {
#ifdef WITH_SKIA
	if (!points || n < 2)
		return;

	SkPathBuilder builder;
	builder.moveTo(points[0].X(), points[0].Y());
	for (size_t i = 1; i < n; ++i)
		builder.lineTo(points[i].X(), points[i].Y());
	GetTargetCanvas().drawPath(builder.detach(), MakeStrokePaint());
#else
	(void)points;
	(void)n;
#endif
}

void SkiaVideoOverlayDrawContext::DrawRectangle(Vector2D p1, Vector2D p2) {
#ifdef WITH_SKIA
	SkRect const rect = SkRect::MakeLTRB(p1.X(), p1.Y(), p2.X(), p2.Y());
	SkCanvas &target = GetTargetCanvas();
	if (fill_alpha > 0.0f)
		target.drawRect(rect, MakeFillPaint());
	if (line_alpha > 0.0f)
		target.drawRect(rect, MakeStrokePaint());
#else
	(void)p1;
	(void)p2;
#endif
}

void SkiaVideoOverlayDrawContext::DrawPolygon(Vector2D const *points, size_t n) {
#ifdef WITH_SKIA
	if (!points || n < 3)
		return;

	SkPathBuilder builder;
	std::vector<SkPoint> polygon;
	polygon.reserve(n);
	for (size_t i = 0; i < n; ++i)
		polygon.push_back(SkPoint::Make(points[i].X(), points[i].Y()));
	builder.addPolygon({ polygon.data(), polygon.size() }, true);
	SkPath const path = builder.detach();
	SkCanvas &target = GetTargetCanvas();
	if (fill_alpha > 0.0f)
		target.drawPath(path, MakeFillPaint());
	if (line_alpha > 0.0f)
		target.drawPath(path, MakeStrokePaint());
#else
	(void)points;
	(void)n;
#endif
}

void SkiaVideoOverlayDrawContext::DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) {
#ifdef WITH_SKIA
	if (points.empty() || start.empty() || count.empty() || start.size() != count.size())
		return;

	SkPathBuilder fill_builder(invert ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
	if (invert)
		fill_builder.addRect(SkRect::MakeXYWH(video_pos.X(), video_pos.Y(), video_size.X(), video_size.Y()));

	std::vector<SkPoint> polygon;
	for (size_t poly = 0; poly < start.size(); ++poly) {
		int const first = start[poly];
		int const point_count = count[poly];
		if (point_count < 3)
			continue;

		polygon.clear();
		polygon.reserve(point_count);
		for (int i = 0; i < point_count; ++i) {
			size_t const offset = static_cast<size_t>(first + i) * 2;
			if (offset + 1 >= points.size())
				break;
			polygon.push_back(SkPoint::Make(points[offset], points[offset + 1]));
		}
		if (polygon.size() >= 3)
			fill_builder.addPolygon({ polygon.data(), polygon.size() }, true);
	}

	SkCanvas &target = GetTargetCanvas();
	SkPath const fill_path = fill_builder.detach();
	if (fill_alpha > 0.0f)
		target.drawPath(fill_path, MakeFillPaint());

	if (line_alpha > 0.0f) {
		SkPaint const stroke = MakeStrokePaint();
		for (size_t poly = 0; poly < start.size(); ++poly) {
			int const first = start[poly];
			int const point_count = count[poly];
			if (point_count < 2)
				continue;

			polygon.clear();
			polygon.reserve(point_count);
			for (int i = 0; i < point_count; ++i) {
				size_t const offset = static_cast<size_t>(first + i) * 2;
				if (offset + 1 >= points.size())
					break;
				polygon.push_back(SkPoint::Make(points[offset], points[offset + 1]));
			}
			if (polygon.size() < 2)
				continue;

			SkPathBuilder outline_builder;
			outline_builder.addPolygon({ polygon.data(), polygon.size() }, true);
			target.drawPath(outline_builder.detach(), stroke);
		}
	}
#else
	(void)points;
	(void)start;
	(void)count;
	(void)video_pos;
	(void)video_size;
	(void)invert;
#endif
}

void SkiaVideoOverlayDrawContext::DrawCircle(Vector2D center, float radius) {
#ifdef WITH_SKIA
	SkCanvas &target = GetTargetCanvas();
	if (fill_alpha > 0.0f)
		target.drawCircle(center.X(), center.Y(), radius, MakeFillPaint());
	if (line_alpha > 0.0f)
		target.drawCircle(center.X(), center.Y(), radius, MakeStrokePaint());
#else
	(void)center;
	(void)radius;
#endif
}

void SkiaVideoOverlayDrawContext::DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) {
#ifdef WITH_SKIA
	SkPathBuilder builder;
	SkPoint points[] = {
		SkPoint::Make(p1.X(), p1.Y()),
		SkPoint::Make(p2.X(), p2.Y()),
		SkPoint::Make(p3.X(), p3.Y())
	};
	builder.addPolygon({ points, 3 }, true);
	SkPath const path = builder.detach();
	SkCanvas &target = GetTargetCanvas();
	if (fill_alpha > 0.0f)
		target.drawPath(path, MakeFillPaint());
	if (line_alpha > 0.0f)
		target.drawPath(path, MakeStrokePaint());
#else
	(void)p1;
	(void)p2;
	(void)p3;
#endif
}

wxSize SkiaVideoOverlayDrawContext::MeasureText(std::string const& value, VideoOverlayTextStyle const& style) {
	return text_cache.MeasureText(value, style);
}

void SkiaVideoOverlayDrawContext::DrawText(std::string const& value, int x, int y, VideoOverlayTextStyle const& style) {
#ifdef WITH_SKIA
	if (invert && invert_canvas) {
		VideoOverlayTextStyle invert_style = style;
		invert_style.colour = *wxWHITE;
		text_cache.DrawText(*invert_canvas, value, x, y, invert_style);
		return;
	}
#endif
	text_cache.DrawText(canvas, value, x, y, style);
}
