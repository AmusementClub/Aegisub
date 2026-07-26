#include "visual_guide_overlay.h"

#include "video_overlay_draw_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace {
constexpr double kDisplayZeroThreshold = 0.0005;
constexpr double kLabelPadding = 4.0;
constexpr double kLabelOffset = 12.0;
// Filled-quad stroke widths in canvas logical pixels. Geometry owns the width
// so the result does not depend on glLineWidth / GL_LINE_SMOOTH quality.
constexpr float kGuideOutlineWidth = 4.0f;
constexpr float kGuideCoreWidth = 2.0f;

bool IsFinite(double value) {
	return std::isfinite(value);
}

bool IsFiniteCanvas(VisualGuideViewport const& viewport) {
	return IsFinite(viewport.canvas_x) && IsFinite(viewport.canvas_y)
		&& IsFinite(viewport.canvas_width) && IsFinite(viewport.canvas_height)
		&& viewport.canvas_width > 0.0 && viewport.canvas_height > 0.0;
}

bool HasScriptDimensions(VisualGuideViewport const& viewport) {
	return IsFinite(viewport.script_width) && IsFinite(viewport.script_height)
		&& viewport.script_width > 0.0 && viewport.script_height > 0.0;
}

void DrawGuideStroke(
	VideoOverlayDrawContext& context,
	Vector2D first,
	Vector2D second,
	wxColour const& colour,
	float alpha,
	float width) {
	// Reject near-zero segments before Unit(). Sub-pixel directions make
	// Perpendicular()*half-width collapse the quad so Skia fill area is zero.
	// FinishInteraction already refuses commits shorter than the hit tolerance;
	// this mainly protects the creation preview.
	constexpr float kMinStrokeSquareLen = 1e-4f;
	Vector2D direction = second - first;
	if (direction.SquareLen() <= kMinStrokeSquareLen || width <= 0.0f || alpha <= 0.0f)
		return;

	Vector2D const unit = direction.Unit();
	if (unit.SquareLen() <= 0.0f)
		return;

	Vector2D const half_normal = unit.Perpendicular() * (width * 0.5f);
	Vector2D const polygon[4] = {
		first + half_normal,
		second + half_normal,
		second - half_normal,
		first - half_normal,
	};

	// Fill only: line alpha 0 skips stroke on both legacy and Skia backends.
	context.SetFillColour(colour, alpha);
	context.SetLineColour(colour, 0.0f, 1);
	context.DrawPolygon(polygon, 4);
}

void DrawGuideLine(
	VideoOverlayDrawContext& context,
	Vector2D first,
	Vector2D second,
	bool selected,
	VisualGuideOverlayStyle const& style) {
	DrawGuideStroke(
		context, first, second, style.outline_colour, 0.85f, kGuideOutlineWidth);
	DrawGuideStroke(
		context,
		first,
		second,
		selected ? style.highlight_colour : style.line_colour,
		1.0f,
		kGuideCoreWidth);
}

std::string TrimFixedNumber(double value) {
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(2) << value;
	auto text = stream.str();
	while (!text.empty() && text.back() == '0')
		text.pop_back();
	if (!text.empty() && text.back() == '.')
		text.pop_back();
	return text.empty() || text == "-0" ? "0" : text;
}

struct MeasurementLabel {
	std::string delta_x;
	std::string delta_y;
	std::string distance;
};

MeasurementLabel BuildMeasurementLabel(VisualGuideMetrics metrics) {
	return {
		std::string("dX ") + FormatVisualGuideLabelNumber(metrics.delta_x, true),
		std::string("dY ") + FormatVisualGuideLabelNumber(metrics.delta_y, true),
		std::string("L ") + FormatVisualGuideLabelNumber(metrics.distance, false),
	};
}

void DrawMeasurementLabel(
	VideoOverlayDrawContext& context,
	VisualGuide const& guide,
	VisualGuideViewport const& viewport,
	Vector2D first,
	Vector2D second,
	VisualGuideOverlayStyle const& style) {
	auto const metrics = CalculateVisualGuideMetrics(guide);
	auto const label = BuildMeasurementLabel(metrics);

	VideoOverlayTextStyle text_style;
	text_style.face = "Verdana";
	text_style.size = std::clamp(style.label_font_size, 6, 72);
	text_style.bold = true;
	text_style.colour = style.line_colour;

	std::array<std::string_view, 3> const metric_lines = {
		label.delta_x, label.delta_y, label.distance,
	};
	std::array<wxSize, 3> extents = {
		context.MeasureText(label.delta_x, text_style),
		context.MeasureText(label.delta_y, text_style),
		context.MeasureText(label.distance, text_style),
	};

	int width = 0;
	int height = 0;
	for (auto const& extent : extents) {
		width = std::max(width, extent.GetWidth());
		height += extent.GetHeight();
	}
	width += static_cast<int>(kLabelPadding * 2.0);
	height += static_cast<int>(kLabelPadding * 2.0);

	Vector2D const midpoint = (first + second) / 2.0;
	Vector2D direction = second - first;
	Vector2D normal(0.0f, -1.0f);
	if (direction.SquareLen() > 0.0)
		normal = Vector2D(-direction.Y(), direction.X()).Unit();

	auto label_origin = [&](Vector2D offset_normal) {
		return midpoint + offset_normal * static_cast<float>(kLabelOffset)
			- Vector2D(width / 2.0f, height / 2.0f);
	};
	Vector2D origin = label_origin(normal);
	auto fits = [&](Vector2D value) {
		return value.X() >= viewport.canvas_x
			&& value.Y() >= viewport.canvas_y
			&& value.X() + width <= viewport.canvas_x + viewport.canvas_width
			&& value.Y() + height <= viewport.canvas_y + viewport.canvas_height;
	};
	if (!fits(origin))
		origin = label_origin(normal * -1.0);

	if (viewport.canvas_width > 0.0 && viewport.canvas_height > 0.0) {
		origin = Vector2D(
			static_cast<float>(std::clamp(
				static_cast<double>(origin.X()),
				viewport.canvas_x,
				std::max(viewport.canvas_x, viewport.canvas_x + viewport.canvas_width - width))),
			static_cast<float>(std::clamp(
				static_cast<double>(origin.Y()),
				viewport.canvas_y,
				std::max(viewport.canvas_y, viewport.canvas_y + viewport.canvas_height - height))));
	}

	Vector2D const background_end = origin + Vector2D(width, height);
	context.SetFillColour(style.label_background_colour, 0.70f);
	context.SetLineColour(style.label_background_colour, 0.0f, 1);
	context.DrawRectangle(origin, background_end);

	int cursor_y = static_cast<int>(std::lround(origin.Y() + kLabelPadding));
	int const text_x = static_cast<int>(std::lround(origin.X() + kLabelPadding));
	for (size_t index = 0; index < metric_lines.size(); ++index) {
		context.DrawText(std::string(metric_lines[index]), text_x, cursor_y, text_style);
		cursor_y += extents[index].GetHeight();
	}
}
}

bool IsVisualGuideViewportMappable(VisualGuideViewport const& viewport) noexcept {
	return IsFiniteCanvas(viewport) && HasScriptDimensions(viewport);
}

Vector2D VisualGuideToCanvas(
	VisualGuidePoint point,
	VisualGuideViewport const& viewport) {
	if (!IsVisualGuideViewportMappable(viewport)
		|| !IsFinite(point.x) || !IsFinite(point.y))
		return {};

	return {
		static_cast<float>(viewport.canvas_x + point.x * viewport.canvas_width / viewport.script_width),
		static_cast<float>(viewport.canvas_y + point.y * viewport.canvas_height / viewport.script_height),
	};
}

std::optional<VisualGuidePoint> CanvasToVisualGuide(
	Vector2D point,
	VisualGuideViewport const& viewport) {
	if (!IsVisualGuideViewportMappable(viewport)
		|| !IsFinite(point.X()) || !IsFinite(point.Y()))
		return std::nullopt;

	return VisualGuidePoint{
		(point.X() - viewport.canvas_x) * viewport.script_width / viewport.canvas_width,
		(point.Y() - viewport.canvas_y) * viewport.script_height / viewport.canvas_height,
	};
}

std::string FormatVisualGuideLabelNumber(double value, bool show_positive_sign) {
	if (!std::isfinite(value) || std::abs(value) < kDisplayZeroThreshold)
		value = 0.0;

	auto text = TrimFixedNumber(value);
	if (show_positive_sign && value > 0.0)
		text.insert(text.begin(), '+');
	return text;
}

void VisualGuideOverlay::Draw(
	VideoOverlayDrawContext& context,
	VisualGuideViewport const& viewport,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideOverlayStyle const& style) const {
	if (!IsFiniteCanvas(viewport))
		return;

	for (auto const& guide : snapshot.guides) {
		bool const selected = snapshot.selected_id && *snapshot.selected_id == guide.id;
		DrawGuide(context, viewport, guide, selected, style);
	}
}

void VisualGuideOverlay::DrawGuide(
	VideoOverlayDrawContext& context,
	VisualGuideViewport const& viewport,
	VisualGuide const& guide,
	bool selected,
	VisualGuideOverlayStyle const& style) const {
	if (!IsVisualGuideViewportMappable(viewport) || !IsValidVisualGuide(guide))
		return;

	Vector2D const first = VisualGuideToCanvas(guide.first, viewport);
	Vector2D const second = VisualGuideToCanvas(guide.second, viewport);
	DrawGuideLine(context, first, second, selected, style);
	DrawMeasurementLabel(context, guide, viewport, first, second, style);
}
