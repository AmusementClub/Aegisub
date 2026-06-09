#include "libaegisub/ass/drawing.h"

#if defined(WITH_DRAWING_SKIA)
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkRect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/pathops/SkPathOps.h"
#endif

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace agi {
namespace ass {
namespace drawing {

#if defined(WITH_DRAWING_SKIA)
namespace {

constexpr SkScalar kStrokeMiterLimit = 2.0f;
constexpr SkScalar kStrokeResScale = 1.0f;
constexpr SkScalar kDashStrokeResScale = 2.0f;
constexpr SkScalar kContourInteractionEpsilon = 1e-6f;
constexpr SkScalar kCanonicalBoundsTolerance = 1.0f / 64.0f;

struct FilledContour {
	SkPath path;
	SkRect bounds {};
};

struct FilledContourCluster {
	int root = 0;
	SkPath path;
	SkRect bounds {};
	int contour_count = 0;
};

struct PendingFilledContourCluster {
	int root = 0;
	SkRect bounds {};
	std::vector<std::size_t> contour_indices;
};

struct CanonicalizedCluster {
	SkPath path;
	std::size_t contour_count = 0;
};

SkPoint ToSkPoint(Point const& point) {
	return SkPoint::Make(static_cast<SkScalar>(point.x), static_cast<SkScalar>(point.y));
}

Point FromSkPoint(SkPoint const& point) {
	return {static_cast<double>(point.x()), static_cast<double>(point.y())};
}

SkPathFillType ToSkFillType(bool winding_fill) {
	return winding_fill ? SkPathFillType::kWinding : SkPathFillType::kEvenOdd;
}

bool IsWindingFill(SkPathFillType fill_type) {
	return fill_type == SkPathFillType::kWinding || fill_type == SkPathFillType::kInverseWinding;
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

SkPath ToSkPath(PathData const& path_data, bool close_implicitly) {
	SkPathBuilder builder(ToSkFillType(path_data.winding_fill));
	bool figure_open = false;
	bool figure_has_segments = false;
	bool figure_explicitly_closed = false;

	auto begin_origin_figure = [&] {
		builder.moveTo(0.0f, 0.0f);
		figure_open = true;
		figure_has_segments = false;
		figure_explicitly_closed = false;
	};

	auto end_figure = [&](bool close_at_boundary) {
		if (!figure_open)
			return;
		if (figure_explicitly_closed || (close_at_boundary && close_implicitly && figure_has_segments))
			builder.close();
		figure_open = false;
		figure_has_segments = false;
		figure_explicitly_closed = false;
	};

	for (auto const& command : path_data.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				end_figure(true);
				builder.moveTo(ToSkPoint(command.p1));
				figure_open = true;
				figure_has_segments = false;
				figure_explicitly_closed = false;
				break;
			case PathVerb::LineTo:
				if (!figure_open)
					begin_origin_figure();
				builder.lineTo(ToSkPoint(command.p1));
				figure_has_segments = true;
				break;
			case PathVerb::QuadTo:
				if (!figure_open)
					begin_origin_figure();
				builder.quadTo(ToSkPoint(command.p1), ToSkPoint(command.p2));
				figure_has_segments = true;
				break;
			case PathVerb::ConicTo:
				if (!figure_open)
					begin_origin_figure();
				builder.conicTo(ToSkPoint(command.p1), ToSkPoint(command.p2), static_cast<SkScalar>(command.weight));
				figure_has_segments = true;
				break;
			case PathVerb::CubicTo:
				if (!figure_open)
					begin_origin_figure();
				builder.cubicTo(ToSkPoint(command.p1), ToSkPoint(command.p2), ToSkPoint(command.p3));
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
	return builder.detach();
}

PathData FromSkPath(SkPath const& sk_path) {
	PathData path;
	path.winding_fill = sk_path.getFillType() != SkPathFillType::kEvenOdd;

	SkPath::Iter iter(sk_path, false);
	SkPoint points[4];
	for (SkPath::Verb verb = iter.next(points); verb != SkPath::kDone_Verb; verb = iter.next(points)) {
		switch (verb) {
			case SkPath::kMove_Verb:
				path.commands.push_back({PathVerb::MoveTo, FromSkPoint(points[0]), {}, {}});
				break;
			case SkPath::kLine_Verb:
				path.commands.push_back({PathVerb::LineTo, FromSkPoint(points[1]), {}, {}});
				break;
			case SkPath::kQuad_Verb:
				path.commands.push_back({PathVerb::QuadTo, FromSkPoint(points[1]), FromSkPoint(points[2]), {}});
				break;
			case SkPath::kConic_Verb:
				path.commands.push_back({
					PathVerb::ConicTo,
					FromSkPoint(points[1]),
					FromSkPoint(points[2]),
					{},
					static_cast<double>(iter.conicWeight()),
				});
				break;
			case SkPath::kCubic_Verb:
				path.commands.push_back({PathVerb::CubicTo, FromSkPoint(points[1]), FromSkPoint(points[2]), FromSkPoint(points[3])});
				break;
			case SkPath::kClose_Verb:
				path.commands.push_back({PathVerb::Close, {}, {}, {}});
				break;
			case SkPath::kDone_Verb:
				break;
		}
	}

	return path;
}

std::optional<SkPath> AsWindingPath(SkPath const& path) {
	SkPath winding;
	if (AsWinding(path, &winding)) {
		winding.setFillType(SkPathFillType::kWinding);
		return winding;
	}

	if (IsWindingFill(path.getFillType())) {
		winding = path;
		winding.setFillType(SkPathFillType::kWinding);
		return winding;
	}

	return std::nullopt;
}

std::vector<FilledContour> SplitClosedFilledContours(SkPath const& path) {
	std::vector<FilledContour> contours;
	SkPathBuilder builder(SkPathFillType::kWinding);
	bool has_contour = false;
	bool contour_has_segments = false;
	bool contour_closed = false;

	auto reset_builder = [&] {
		builder = SkPathBuilder(SkPathFillType::kWinding);
		has_contour = false;
		contour_has_segments = false;
		contour_closed = false;
	};

	auto ensure_contour = [&](SkPoint const& start) {
		if (has_contour)
			return;
		builder.moveTo(start);
		has_contour = true;
	};

	auto flush_contour = [&] {
		if (!has_contour)
			return;

		if (contour_has_segments) {
			if (!contour_closed)
				builder.close();

			SkPath contour = builder.detach();
			contour.setFillType(SkPathFillType::kWinding);
			if (!contour.isEmpty())
				contours.push_back({contour, contour.getBounds()});
		}

		reset_builder();
	};

	SkPath::Iter iter(path, false);
	SkPoint points[4];
	for (SkPath::Verb verb = iter.next(points); verb != SkPath::kDone_Verb; verb = iter.next(points)) {
		switch (verb) {
			case SkPath::kMove_Verb:
				flush_contour();
				builder.moveTo(points[0]);
				has_contour = true;
				break;
			case SkPath::kLine_Verb:
				ensure_contour(points[0]);
				builder.lineTo(points[1]);
				contour_has_segments = true;
				break;
			case SkPath::kQuad_Verb:
				ensure_contour(points[0]);
				builder.quadTo(points[1], points[2]);
				contour_has_segments = true;
				break;
			case SkPath::kConic_Verb:
				ensure_contour(points[0]);
				builder.conicTo(points[1], points[2], iter.conicWeight());
				contour_has_segments = true;
				break;
			case SkPath::kCubic_Verb:
				ensure_contour(points[0]);
				builder.cubicTo(points[1], points[2], points[3]);
				contour_has_segments = true;
				break;
			case SkPath::kClose_Verb:
				if (has_contour) {
					builder.close();
					contour_closed = true;
					flush_contour();
				}
				break;
			case SkPath::kDone_Verb:
				break;
		}
	}

	flush_contour();
	return contours;
}

bool BoundsMayInteract(SkRect const& lhs, SkRect const& rhs) {
	return lhs.left() <= rhs.right() + kContourInteractionEpsilon &&
		rhs.left() <= lhs.right() + kContourInteractionEpsilon &&
		lhs.top() <= rhs.bottom() + kContourInteractionEpsilon &&
		rhs.top() <= lhs.bottom() + kContourInteractionEpsilon;
}

bool FilledContoursMayInteract(FilledContour const& lhs, FilledContour const& rhs) {
	if (!BoundsMayInteract(lhs.bounds, rhs.bounds))
		return false;

	SkPath intersection;
	if (!Op(lhs.path, rhs.path, kIntersect_SkPathOp, &intersection))
		return true;

	return !intersection.isEmpty();
}

std::vector<FilledContourCluster> BuildFilledContourClusters(std::vector<FilledContour> const& contours) {
	if (contours.empty())
		return {};

	std::vector<int> parent(contours.size());
	for (std::size_t index = 0; index < parent.size(); ++index)
		parent[index] = static_cast<int>(index);

	auto find_root = [&](int value) {
		int root = value;
		while (parent[root] != root)
			root = parent[root];
		while (parent[value] != value) {
			int next = parent[value];
			parent[value] = root;
			value = next;
		}
		return root;
	};

	auto unite_roots = [&](int lhs, int rhs) {
		int lhs_root = find_root(lhs);
		int rhs_root = find_root(rhs);
		if (lhs_root != rhs_root)
			parent[rhs_root] = lhs_root;
	};

	std::vector<std::size_t> order(contours.size());
	for (std::size_t index = 0; index < order.size(); ++index)
		order[index] = index;

	std::sort(order.begin(), order.end(), [&contours](std::size_t lhs, std::size_t rhs) {
		return contours[lhs].bounds.left() < contours[rhs].bounds.left();
	});

	std::vector<std::size_t> active;
	active.reserve(contours.size());
	for (std::size_t rhs : order) {
		SkScalar left_limit = contours[rhs].bounds.left() - kContourInteractionEpsilon;
		active.erase(std::remove_if(active.begin(), active.end(), [&contours, left_limit](std::size_t lhs) {
			return contours[lhs].bounds.right() < left_limit;
		}), active.end());

		for (std::size_t lhs : active) {
			if (FilledContoursMayInteract(contours[lhs], contours[rhs]))
				unite_roots(static_cast<int>(lhs), static_cast<int>(rhs));
		}
		active.push_back(rhs);
	}

	std::vector<PendingFilledContourCluster> pending;
	std::vector<int> cluster_by_root(contours.size(), -1);
	for (std::size_t index = 0; index < contours.size(); ++index) {
		int root = find_root(static_cast<int>(index));
		int cluster_index = cluster_by_root[root];
		if (cluster_index < 0) {
			PendingFilledContourCluster created;
			created.root = root;
			created.bounds = contours[index].bounds;
			created.contour_indices.push_back(index);
			cluster_by_root[root] = static_cast<int>(pending.size());
			pending.push_back(std::move(created));
			continue;
		}

		auto& cluster = pending[cluster_index];
		cluster.bounds.join(contours[index].bounds);
		cluster.contour_indices.push_back(index);
	}

	std::vector<FilledContourCluster> clusters;
	clusters.reserve(pending.size());
	for (auto const& source : pending) {
		SkPathBuilder builder(SkPathFillType::kWinding);
		for (std::size_t index : source.contour_indices)
			builder.addPath(contours[index].path, SkPath::kAppend_AddPathMode);

		SkPath path = builder.detach();
		path.setFillType(SkPathFillType::kWinding);

		FilledContourCluster cluster;
		cluster.root = source.root;
		cluster.path = path;
		cluster.bounds = source.bounds;
		cluster.contour_count = static_cast<int>(source.contour_indices.size());
		clusters.push_back(std::move(cluster));
	}

	return clusters;
}

std::size_t CountFilledContours(SkPath const& path) {
	return SplitClosedFilledContours(path).size();
}

std::optional<SkPath> CanonicalizeFilledPathWithPathOps(SkPath const& path) {
	if (path.isEmpty()) {
		SkPath empty;
		empty.setFillType(SkPathFillType::kWinding);
		return empty;
	}

	SkPath canonical;
	if (Op(path, SkPath(), kUnion_SkPathOp, &canonical)) {
		if (auto winding = AsWindingPath(canonical))
			return winding;
	}

	if (Simplify(path, &canonical)) {
		if (auto winding = AsWindingPath(canonical))
			return winding;
	}

	return AsWindingPath(path);
}

bool BoundsCloseEnough(SkRect const& lhs, SkRect const& rhs) {
	return std::abs(lhs.left() - rhs.left()) <= kCanonicalBoundsTolerance &&
		std::abs(lhs.top() - rhs.top()) <= kCanonicalBoundsTolerance &&
		std::abs(lhs.right() - rhs.right()) <= kCanonicalBoundsTolerance &&
		std::abs(lhs.bottom() - rhs.bottom()) <= kCanonicalBoundsTolerance;
}

std::optional<CanonicalizedCluster> CanonicalizeFilledContourCluster(FilledContourCluster const& cluster) {
	if (cluster.contour_count <= 1) {
		if (cluster.path.isEmpty())
			return std::nullopt;

		SkPath preserved = cluster.path;
		preserved.setFillType(SkPathFillType::kWinding);
		return CanonicalizedCluster{preserved, 1};
	}

	auto canonical = CanonicalizeFilledPathWithPathOps(cluster.path);
	if (!canonical)
		return std::nullopt;

	std::size_t canonical_contours = CountFilledContours(*canonical);
	if (!BoundsCloseEnough(canonical->getBounds(), cluster.bounds) ||
		canonical_contours != static_cast<std::size_t>(cluster.contour_count)) {
		SkPath preserved = cluster.path;
		preserved.setFillType(SkPathFillType::kWinding);
		return CanonicalizedCluster{preserved, static_cast<std::size_t>(cluster.contour_count)};
	}

	return CanonicalizedCluster{*canonical, canonical_contours};
}

std::optional<SkPath> CanonicalizeFilledPathByContourClusters(SkPath const& path) {
	auto contours = SplitClosedFilledContours(path);
	if (contours.empty())
		return std::nullopt;

	if (contours.size() == 1) {
		SkPath preserved = contours.front().path;
		preserved.setFillType(SkPathFillType::kWinding);
		return preserved;
	}

	auto clusters = BuildFilledContourClusters(contours);
	SkPathBuilder builder(SkPathFillType::kWinding);
	for (auto const& cluster : clusters) {
		auto canonical = CanonicalizeFilledContourCluster(cluster);
		if (!canonical)
			return std::nullopt;
		builder.addPath(canonical->path, SkPath::kAppend_AddPathMode);
	}

	SkPath result = builder.detach();
	if (result.isEmpty())
		return std::nullopt;

	result.setFillType(SkPathFillType::kWinding);
	return result;
}

std::optional<SkPath> CanonicalizeFilledPath(SkPath const& path) {
	if (path.isEmpty()) {
		SkPath empty;
		empty.setFillType(SkPathFillType::kWinding);
		return empty;
	}

	if (auto clustered = CanonicalizeFilledPathByContourClusters(path))
		return clustered;

	return CanonicalizeFilledPathWithPathOps(path);
}

PathData FromCanonicalOrWindingFallback(SkPath const& sk_path, std::optional<SkPath> canonical) {
	if (canonical)
		return FromSkPath(*canonical);

	SkPath fallback = sk_path;
	fallback.setFillType(SkPathFillType::kWinding);
	return FromSkPath(fallback);
}

PathData FromSkPathForAssExport(SkPath const& sk_path) {
	return FromCanonicalOrWindingFallback(sk_path, CanonicalizeFilledPath(sk_path));
}

PathData FromSkPathForBooleanExport(SkPath const& sk_path) {
	return FromCanonicalOrWindingFallback(sk_path, CanonicalizeFilledPathWithPathOps(sk_path));
}

bool StrokePath(SkPath const& source,
	double width,
	DrawingStrokeCap cap,
	DrawingStrokeJoin join,
	SkScalar res_scale,
	sk_sp<SkPathEffect> effect,
	SkPath& stroked) {
	if (width <= 0.0)
		return false;

	SkPaint paint;
	paint.setStyle(SkPaint::kStroke_Style);
	paint.setStrokeWidth(static_cast<SkScalar>(width));
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

bool DrawingSkiaBackendAvailable() {
#if defined(WITH_DRAWING_SKIA)
	return true;
#else
	return false;
#endif
}

bool TryDrawingContainsPoint(PathData const& path, double x, double y, bool& contains) {
#if defined(WITH_DRAWING_SKIA)
	contains = ToSkPath(path, true).contains(static_cast<SkScalar>(x), static_cast<SkScalar>(y));
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
	if (width <= 0.0 || height <= 0.0) {
		contains = false;
		return true;
	}

	SkPath shape = ToSkPath(path, true);
	SkRect rect = SkRect::MakeXYWH(static_cast<SkScalar>(x), static_cast<SkScalar>(y), static_cast<SkScalar>(width), static_cast<SkScalar>(height));
	if (!shape.getBounds().contains(rect)) {
		contains = false;
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
	SkPath sk_result;
	if (!Op(ToSkPath(lhs, true), ToSkPath(rhs, true), ToSkPathOp(op), &sk_result))
		return false;
	result = FromSkPathForBooleanExport(sk_result);
	return true;
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
	if (width <= 0.0) {
		result = {};
		return true;
	}

	SkPath stroked;
	if (!StrokePath(ToSkPath(path, false), width, cap, join, kStrokeResScale, nullptr, stroked))
		return false;
	result = FromSkPathForAssExport(stroked);
	return true;
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
	if (width <= 0.0 || pattern_length <= 0.0 || space_length < 0.0) {
		result = {};
		return true;
	}

	SkScalar intervals[] = {
		static_cast<SkScalar>(pattern_length * width),
		static_cast<SkScalar>(space_length * width),
	};
	auto dash = SkDashPathEffect::Make(intervals, static_cast<SkScalar>(dash_offset * width));
	if (!dash)
		return false;

	SkPath stroked;
	if (!StrokePath(ToSkPath(path, false), width, cap, join, kDashStrokeResScale, std::move(dash), stroked))
		return false;
	result = FromSkPathForAssExport(stroked);
	return true;
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
