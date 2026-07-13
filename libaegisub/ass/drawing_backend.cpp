#include "libaegisub/ass/drawing.h"

#if defined(WITH_DRAWING_SKIA)
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkRect.h"
#include "include/core/SkSpan.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/pathops/SkPathOps.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace agi {
namespace ass {
namespace drawing {

#if defined(WITH_DRAWING_SKIA)
namespace {

constexpr SkScalar kStrokeMiterLimit = 2.0f;
constexpr SkScalar kStrokeResScale = 1.0f;
constexpr SkScalar kDashStrokeResScale = 2.0f;

struct BackendDomain {
	bool has_point = false;
	double min_x = std::numeric_limits<double>::infinity();
	double min_y = std::numeric_limits<double>::infinity();
	double max_x = -std::numeric_limits<double>::infinity();
	double max_y = -std::numeric_limits<double>::infinity();

	bool Include(Point const& point) {
		if (!std::isfinite(point.x) || !std::isfinite(point.y))
			return false;
		has_point = true;
		min_x = std::min(min_x, point.x);
		min_y = std::min(min_y, point.y);
		max_x = std::max(max_x, point.x);
		max_y = std::max(max_y, point.y);
		return true;
	}
};

bool ExtendBackendDomain(BackendDomain& domain, PathData const& path) {
	bool has_current = false;
	for (auto const& command : path.commands) {
		auto ensure_implicit_origin = [&] {
			if (has_current)
				return true;
			has_current = true;
			return domain.Include({});
		};

		switch (command.verb) {
			case PathVerb::MoveTo:
				has_current = true;
				if (!domain.Include(command.p1))
					return false;
				break;
			case PathVerb::LineTo:
				if (!ensure_implicit_origin() || !domain.Include(command.p1))
					return false;
				break;
			case PathVerb::QuadTo:
				if (!ensure_implicit_origin() || !domain.Include(command.p1) || !domain.Include(command.p2))
					return false;
				break;
			case PathVerb::ConicTo:
				if (!ensure_implicit_origin() || !domain.Include(command.p1) || !domain.Include(command.p2) ||
					!(command.weight > 0.0) || !std::isfinite(command.weight))
					return false;
				break;
			case PathVerb::CubicTo:
				if (!ensure_implicit_origin() || !domain.Include(command.p1) || !domain.Include(command.p2) || !domain.Include(command.p3))
					return false;
				break;
			case PathVerb::Close:
				break;
		}
	}
	return true;
}

struct BackendCoordinates {
	Point origin {};

	bool ToSkPoint(Point const& point, SkPoint& converted) const {
		double local_x = point.x - origin.x;
		double local_y = point.y - origin.y;
		SkScalar x = static_cast<SkScalar>(local_x);
		SkScalar y = static_cast<SkScalar>(local_y);
		if (!std::isfinite(local_x) || !std::isfinite(local_y) || !std::isfinite(x) || !std::isfinite(y))
			return false;
		converted = SkPoint::Make(x, y);
		return true;
	}

	bool FromSkPoint(SkPoint const& point, Point& converted) const {
		converted = {
			origin.x + static_cast<double>(point.x()),
			origin.y + static_cast<double>(point.y()),
		};
		return std::isfinite(converted.x) && std::isfinite(converted.y);
	}
};

bool MakeBackendCoordinates(BackendDomain const& domain, BackendCoordinates& coordinates) {
	if (!domain.has_point) {
		coordinates = {};
		return true;
	}

	// SkScalar is normally float. Removing the common translation before the
	// conversion preserves ASS's 1/64 grid for small geometry placed at very
	// large absolute coordinates. Half-sums avoid overflowing wide domains.
	coordinates.origin = {
		domain.min_x * 0.5 + domain.max_x * 0.5,
		domain.min_y * 0.5 + domain.max_y * 0.5,
	};
	if (!std::isfinite(coordinates.origin.x) || !std::isfinite(coordinates.origin.y))
		return false;

	SkPoint ignored;
	return coordinates.ToSkPoint({domain.min_x, domain.min_y}, ignored) &&
		coordinates.ToSkPoint({domain.max_x, domain.max_y}, ignored);
}

SkPathFillType ToSkFillType(bool winding_fill) {
	return winding_fill ? SkPathFillType::kWinding : SkPathFillType::kEvenOdd;
}

SkPaint::Cap ToSkCap(DrawingStrokeCap cap) {
	switch (cap) {
		case DrawingStrokeCap::Flat:
			return SkPaint::kButt_Cap;
		case DrawingStrokeCap::Round:
			return SkPaint::kRound_Cap;
		case DrawingStrokeCap::Square:
			return SkPaint::kSquare_Cap;
	}
	return SkPaint::kSquare_Cap;
}

SkPaint::Join ToSkJoin(DrawingStrokeJoin join) {
	switch (join) {
		case DrawingStrokeJoin::Miter:
		case DrawingStrokeJoin::SvgMiter:
			return SkPaint::kMiter_Join;
		case DrawingStrokeJoin::Bevel:
			return SkPaint::kBevel_Join;
		case DrawingStrokeJoin::Round:
			return SkPaint::kRound_Join;
	}
	return SkPaint::kBevel_Join;
}

SkPathOp ToSkPathOp(DrawingBooleanOp op) {
	switch (op) {
		case DrawingBooleanOp::Union:
			return kUnion_SkPathOp;
		case DrawingBooleanOp::Intersect:
			return kIntersect_SkPathOp;
		case DrawingBooleanOp::Subtract:
			return kDifference_SkPathOp;
		case DrawingBooleanOp::Xor:
			return kXOR_SkPathOp;
	}
	return kUnion_SkPathOp;
}

bool ToSkPath(PathData const& path_data,
	bool close_implicitly,
	BackendCoordinates const& coordinates,
	SkPath& converted_path) {
	SkPathBuilder builder(ToSkFillType(path_data.winding_fill));
	bool figure_open = false;
	bool figure_has_segments = false;
	bool figure_explicitly_closed = false;
	bool has_current_point = false;
	SkPoint current_point = SkPoint::Make(0.0f, 0.0f);
	SkPoint figure_start = current_point;

	auto begin_current_figure = [&] {
		if (!has_current_point) {
			if (!coordinates.ToSkPoint({}, current_point))
				return false;
			has_current_point = true;
		}
		builder.moveTo(current_point);
		figure_start = current_point;
		figure_open = true;
		figure_has_segments = false;
		figure_explicitly_closed = false;
		return true;
	};

	auto end_figure = [&](bool close_at_boundary) {
		if (!figure_open)
			return;
		if (figure_explicitly_closed || (close_at_boundary && close_implicitly && figure_has_segments)) {
			builder.close();
			current_point = figure_start;
		}
		figure_open = false;
		figure_has_segments = false;
		figure_explicitly_closed = false;
	};

	for (auto const& command : path_data.commands) {
		SkPoint p1;
		SkPoint p2;
		SkPoint p3;
		switch (command.verb) {
			case PathVerb::MoveTo:
				end_figure(true);
				if (!coordinates.ToSkPoint(command.p1, current_point))
					return false;
				has_current_point = true;
				figure_start = current_point;
				builder.moveTo(current_point);
				figure_open = true;
				figure_has_segments = false;
				figure_explicitly_closed = false;
				break;
			case PathVerb::LineTo:
				if (!figure_open && !begin_current_figure())
					return false;
				if (!coordinates.ToSkPoint(command.p1, current_point))
					return false;
				builder.lineTo(current_point);
				figure_has_segments = true;
				break;
			case PathVerb::QuadTo:
				if (!figure_open && !begin_current_figure())
					return false;
				if (!coordinates.ToSkPoint(command.p1, p1) || !coordinates.ToSkPoint(command.p2, p2))
					return false;
				builder.quadTo(p1, p2);
				current_point = p2;
				figure_has_segments = true;
				break;
			case PathVerb::ConicTo:
				if (!figure_open && !begin_current_figure())
					return false;
				if (!coordinates.ToSkPoint(command.p1, p1) || !coordinates.ToSkPoint(command.p2, p2))
					return false;
				{
					SkScalar weight = static_cast<SkScalar>(command.weight);
					if (!(weight > 0.0f) || !std::isfinite(weight))
						return false;
					builder.conicTo(p1, p2, weight);
				}
				current_point = p2;
				figure_has_segments = true;
				break;
			case PathVerb::CubicTo:
				if (!figure_open && !begin_current_figure())
					return false;
				if (!coordinates.ToSkPoint(command.p1, p1) || !coordinates.ToSkPoint(command.p2, p2) ||
					!coordinates.ToSkPoint(command.p3, p3))
					return false;
				builder.cubicTo(p1, p2, p3);
				current_point = p3;
				figure_has_segments = true;
				break;
			case PathVerb::Close:
				if (figure_open) {
					figure_explicitly_closed = true;
					end_figure(false);
				}
				break;
		}
	}

	end_figure(true);
	converted_path = builder.detach();
	return true;
}

bool FromSkPath(SkPath const& sk_path, BackendCoordinates const& coordinates, PathData& result) {
	PathData path;
	path.winding_fill = sk_path.getFillType() != SkPathFillType::kEvenOdd;

	SkPath::Iter iter(sk_path, false);
	SkPoint points[4];
	for (SkPath::Verb verb = iter.next(points); verb != SkPath::kDone_Verb; verb = iter.next(points)) {
		Point p1;
		Point p2;
		Point p3;
		switch (verb) {
			case SkPath::kMove_Verb:
				if (!coordinates.FromSkPoint(points[0], p1))
					return false;
				path.commands.push_back({PathVerb::MoveTo, p1, {}, {}});
				break;
			case SkPath::kLine_Verb:
				if (!coordinates.FromSkPoint(points[1], p1))
					return false;
				path.commands.push_back({PathVerb::LineTo, p1, {}, {}});
				break;
			case SkPath::kQuad_Verb:
				if (!coordinates.FromSkPoint(points[1], p1) || !coordinates.FromSkPoint(points[2], p2))
					return false;
				path.commands.push_back({PathVerb::QuadTo, p1, p2, {}});
				break;
			case SkPath::kConic_Verb:
				if (!coordinates.FromSkPoint(points[1], p1) || !coordinates.FromSkPoint(points[2], p2) ||
					!(iter.conicWeight() > 0.0f) || !std::isfinite(iter.conicWeight()))
					return false;
				path.commands.push_back({
					PathVerb::ConicTo,
					p1,
					p2,
					{},
					static_cast<double>(iter.conicWeight()),
				});
				break;
			case SkPath::kCubic_Verb:
				if (!coordinates.FromSkPoint(points[1], p1) || !coordinates.FromSkPoint(points[2], p2) ||
					!coordinates.FromSkPoint(points[3], p3))
					return false;
				path.commands.push_back({PathVerb::CubicTo, p1, p2, p3});
				break;
			case SkPath::kClose_Verb:
				path.commands.push_back({PathVerb::Close, {}, {}, {}});
				break;
			case SkPath::kDone_Verb:
				break;
		}
	}

	result = std::move(path);
	return true;
}

bool PrepareBackendCoordinates(PathData const& path, BackendCoordinates& coordinates) {
	BackendDomain domain;
	return ExtendBackendDomain(domain, path) && MakeBackendCoordinates(domain, coordinates);
}

bool PrepareBackendCoordinates(PathData const& lhs, PathData const& rhs, BackendCoordinates& coordinates) {
	BackendDomain domain;
	return ExtendBackendDomain(domain, lhs) && ExtendBackendDomain(domain, rhs) &&
		MakeBackendCoordinates(domain, coordinates);
}

bool StrokePath(SkPath const& source,
	double width,
	DrawingStrokeCap cap,
	DrawingStrokeJoin join,
	SkScalar res_scale,
	sk_sp<SkPathEffect> effect,
	SkPath& stroked) {
	SkScalar stroke_width = static_cast<SkScalar>(width);
	if (!(width > 0.0) || !std::isfinite(width) || !std::isfinite(stroke_width))
		return false;

	SkPaint paint;
	paint.setStyle(SkPaint::kStroke_Style);
	paint.setStrokeWidth(stroke_width);
	paint.setStrokeCap(ToSkCap(cap));
	paint.setStrokeJoin(ToSkJoin(join));
	paint.setStrokeMiter(kStrokeMiterLimit);
	paint.setPathEffect(std::move(effect));

	SkPathBuilder builder(source.getFillType());
	SkMatrix ctm = SkMatrix::Scale(res_scale, res_scale);
	if (!skpathutils::FillPathWithPaint(source, paint, &builder, nullptr, ctm))
		return false;

	stroked = builder.detach();
	stroked.setFillType(SkPathFillType::kWinding);
	return true;
}

} // namespace
#endif

bool DrawingBackendAvailable() {
#if defined(WITH_DRAWING_SKIA)
	return true;
#else
	return false;
#endif
}

bool DrawingSkiaBackendAvailable() {
	return DrawingBackendAvailable();
}

bool TryDrawingContainsPoint(PathData const& path, double x, double y, bool& contains) {
#if defined(WITH_DRAWING_SKIA)
	contains = false;
	if (!std::isfinite(x) || !std::isfinite(y))
		return false;

	BackendCoordinates coordinates;
	SkPath shape;
	if (!PrepareBackendCoordinates(path, coordinates) || !ToSkPath(path, true, coordinates, shape))
		return false;

	SkPoint query;
	if (!coordinates.ToSkPoint({x, y}, query))
		return true;
	contains = shape.contains(query.x(), query.y());
	return true;
#else
	(void)path;
	(void)x;
	(void)y;
	contains = false;
	return false;
#endif
}

bool TryDrawingContainsRect(PathData const& path, double x, double y, double width, double height, bool& contains) {
#if defined(WITH_DRAWING_SKIA)
	contains = false;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height))
		return false;
	if (width <= 0.0 || height <= 0.0) {
		return true;
	}

	BackendCoordinates coordinates;
	SkPath shape;
	if (!PrepareBackendCoordinates(path, coordinates) || !ToSkPath(path, true, coordinates, shape))
		return false;

	SkPoint top_left;
	SkScalar sk_width = static_cast<SkScalar>(width);
	SkScalar sk_height = static_cast<SkScalar>(height);
	if (!coordinates.ToSkPoint({x, y}, top_left))
		return true;
	if (!(sk_width > 0.0f) || !(sk_height > 0.0f) || !std::isfinite(sk_width) || !std::isfinite(sk_height))
		return false;

	SkRect rect = SkRect::MakeXYWH(top_left.x(), top_left.y(), sk_width, sk_height);
	if (!shape.getBounds().contains(rect)) {
		return true;
	}

	SkPathBuilder rect_builder(ToSkFillType(true));
	rect_builder.addRect(rect);
	SkPath difference;
	if (!Op(rect_builder.detach(), shape, kDifference_SkPathOp, &difference))
		return false;
	contains = difference.isEmpty();
	return true;
#else
	(void)path;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	contains = false;
	return false;
#endif
}

bool TryDrawingBoolean(PathData const& lhs, PathData const& rhs, DrawingBooleanOp op, PathData& result) {
#if defined(WITH_DRAWING_SKIA)
	result = {};
	BackendCoordinates coordinates;
	SkPath sk_lhs;
	SkPath sk_rhs;
	if (!PrepareBackendCoordinates(lhs, rhs, coordinates) ||
		!ToSkPath(lhs, true, coordinates, sk_lhs) || !ToSkPath(rhs, true, coordinates, sk_rhs))
		return false;

	SkPath sk_result;
	if (!Op(sk_lhs, sk_rhs, ToSkPathOp(op), &sk_result))
		return false;
	// PathOps can return even-odd contours which touch at shared vertices.
	// Simplify first produces equivalent non-overlapping contours; direct
	// AsWinding on the touching XOR representation can fill its intended hole.
	SkPath simplified;
	if (!Simplify(sk_result, &simplified))
		return false;
	auto winding_result = AsWinding(simplified);
	if (!winding_result)
		return false;
	return FromSkPath(*winding_result, coordinates, result);
#else
	(void)lhs;
	(void)rhs;
	(void)op;
	result = {};
	return false;
#endif
}

bool TryDrawingOutline(PathData const& path, double width, DrawingStrokeCap cap, DrawingStrokeJoin join, PathData& result) {
#if defined(WITH_DRAWING_SKIA)
	result = {};
	if (!std::isfinite(width))
		return false;
	if (width < 0.0)
		return false;
	if (width == 0.0 || path.commands.empty()) {
		return true;
	}

	BackendCoordinates coordinates;
	SkPath source;
	if (!PrepareBackendCoordinates(path, coordinates) || !ToSkPath(path, false, coordinates, source))
		return false;

	SkPath stroked;
	if (!StrokePath(source, width, cap, join, kStrokeResScale, nullptr, stroked))
		return false;
	return FromSkPath(stroked, coordinates, result);
#else
	(void)path;
	(void)width;
	(void)cap;
	(void)join;
	result = {};
	return false;
#endif
}

bool TryDrawingPatternOutline(PathData const& path,
	double width,
	DrawingStrokeCap cap,
	DrawingStrokeJoin join,
	double pattern_length,
	double space_length,
	double dash_offset,
	PathData& result) {
#if defined(WITH_DRAWING_SKIA)
	result = {};
	if (!std::isfinite(width) || !std::isfinite(pattern_length) || !std::isfinite(space_length) || !std::isfinite(dash_offset))
		return false;
	if (width < 0.0 || pattern_length < 0.0 || space_length < 0.0)
		return false;
	if (width == 0.0 || pattern_length == 0.0 || path.commands.empty()) {
		return true;
	}
	if (space_length == 0.0)
		return TryDrawingOutline(path, width, cap, join, result);

	double interval_on = pattern_length * width;
	double interval_off = space_length * width;
	double phase = dash_offset * width;
	SkScalar intervals[] = {
		static_cast<SkScalar>(interval_on),
		static_cast<SkScalar>(interval_off),
	};
	SkScalar sk_phase = static_cast<SkScalar>(phase);
	if (!std::isfinite(interval_on) || !std::isfinite(interval_off) || !std::isfinite(phase) ||
		!std::isfinite(intervals[0]) || !std::isfinite(intervals[1]) || !std::isfinite(sk_phase))
		return false;
	auto dash = SkDashPathEffect::Make(SkSpan<const SkScalar>(intervals, 2), sk_phase);
	if (!dash)
		return false;

	BackendCoordinates coordinates;
	SkPath source;
	if (!PrepareBackendCoordinates(path, coordinates) || !ToSkPath(path, false, coordinates, source))
		return false;

	SkPath stroked;
	if (!StrokePath(source, width, cap, join, kDashStrokeResScale, std::move(dash), stroked))
		return false;
	return FromSkPath(stroked, coordinates, result);
#else
	(void)path;
	(void)width;
	(void)cap;
	(void)join;
	(void)pattern_length;
	(void)space_length;
	(void)dash_offset;
	result = {};
	return false;
#endif
}

} // namespace drawing
} // namespace ass
} // namespace agi
