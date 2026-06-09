#include "libaegisub/ass/drawing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace agi {
namespace ass {
namespace drawing {

namespace {

constexpr double kDefaultFlattenTolerance = 0.25;
constexpr double kMinimumFlattenTolerance = 1.0 / 1024.0;
constexpr double kLengthEpsilon = 1e-9;
constexpr double kLengthIntegrationEpsilon = 1e-5;
constexpr int kMaxFlattenDepth = 16;
constexpr int kLengthIntegrationMaxDepth = 12;
constexpr int kLengthInverseIterations = 32;

struct BoundsAccumulator {
	bool has_point = false;
	double min_x = std::numeric_limits<double>::infinity();
	double min_y = std::numeric_limits<double>::infinity();
	double max_x = -std::numeric_limits<double>::infinity();
	double max_y = -std::numeric_limits<double>::infinity();
};

struct CubicBezier {
	Point p0 {};
	Point p1 {};
	Point p2 {};
	Point p3 {};
};

struct Segment {
	PathVerb verb = PathVerb::LineTo;
	Point start {};
	Point c1 {};
	Point c2 {};
	Point end {};
};

struct Contour {
	bool has_anchor = false;
	Point anchor {};
	std::vector<Segment> segments;
};

struct MeasuredSegment {
	bool is_line = true;
	CubicBezier cubic {};
	double length = 0.0;
};

struct MeasuredPath {
	std::vector<MeasuredSegment> segments;
	double total_length = 0.0;
};

bool PointIsFinite(Point const& point) {
	return std::isfinite(point.x) && std::isfinite(point.y);
}

void IncludePoint(BoundsAccumulator& bounds, Point const& point) {
	if (!PointIsFinite(point))
		return;

	bounds.has_point = true;
	bounds.min_x = std::min(bounds.min_x, point.x);
	bounds.min_y = std::min(bounds.min_y, point.y);
	bounds.max_x = std::max(bounds.max_x, point.x);
	bounds.max_y = std::max(bounds.max_y, point.y);
}

Point CubicPointAt(CubicBezier const& cubic, double t) {
	double mt = 1.0 - t;
	double mt2 = mt * mt;
	double t2 = t * t;
	return {
		cubic.p0.x * mt2 * mt + 3.0 * cubic.p1.x * mt2 * t + 3.0 * cubic.p2.x * mt * t2 + cubic.p3.x * t2 * t,
		cubic.p0.y * mt2 * mt + 3.0 * cubic.p1.y * mt2 * t + 3.0 * cubic.p2.y * mt * t2 + cubic.p3.y * t2 * t,
	};
}

Point CubicDerivativeAt(CubicBezier const& cubic, double t) {
	double mt = 1.0 - t;
	double a = 3.0 * mt * mt;
	double b = 6.0 * mt * t;
	double c = 3.0 * t * t;

	return {
		a * (cubic.p1.x - cubic.p0.x) + b * (cubic.p2.x - cubic.p1.x) + c * (cubic.p3.x - cubic.p2.x),
		a * (cubic.p1.y - cubic.p0.y) + b * (cubic.p2.y - cubic.p1.y) + c * (cubic.p3.y - cubic.p2.y),
	};
}

Point Lerp(Point const& lhs, Point const& rhs, double t) {
	return {
		lhs.x + (rhs.x - lhs.x) * t,
		lhs.y + (rhs.y - lhs.y) * t,
	};
}

void IncludeCubicRoot(BoundsAccumulator& bounds, CubicBezier const& cubic, double t) {
	if (t > 0.0 && t < 1.0 && std::isfinite(t))
		IncludePoint(bounds, CubicPointAt(cubic, t));
}

void IncludeCubicCoordinateExtrema(BoundsAccumulator& bounds, CubicBezier const& cubic, double p0, double p1, double p2, double p3) {
	double a = -p0 + 3.0 * p1 - 3.0 * p2 + p3;
	double b = 3.0 * p0 - 6.0 * p1 + 3.0 * p2;
	double c = -3.0 * p0 + 3.0 * p1;

	double derivative_a = 3.0 * a;
	double derivative_b = 2.0 * b;
	double derivative_c = c;

	if (std::abs(derivative_a) <= kPointEpsilon) {
		if (std::abs(derivative_b) > kPointEpsilon)
			IncludeCubicRoot(bounds, cubic, -derivative_c / derivative_b);
		return;
	}

	double discriminant = derivative_b * derivative_b - 4.0 * derivative_a * derivative_c;
	if (discriminant < 0.0)
		return;

	double root = std::sqrt(discriminant);
	IncludeCubicRoot(bounds, cubic, (-derivative_b - root) / (2.0 * derivative_a));
	IncludeCubicRoot(bounds, cubic, (-derivative_b + root) / (2.0 * derivative_a));
}

void IncludeCubicBounds(BoundsAccumulator& bounds, CubicBezier const& cubic) {
	IncludePoint(bounds, cubic.p0);
	IncludePoint(bounds, cubic.p3);
	IncludeCubicCoordinateExtrema(bounds, cubic, cubic.p0.x, cubic.p1.x, cubic.p2.x, cubic.p3.x);
	IncludeCubicCoordinateExtrema(bounds, cubic, cubic.p0.y, cubic.p1.y, cubic.p2.y, cubic.p3.y);
}

double DistanceSquared(Point const& lhs, Point const& rhs) {
	double dx = lhs.x - rhs.x;
	double dy = lhs.y - rhs.y;
	return dx * dx + dy * dy;
}

double LineLength(Point const& start, Point const& end) {
	return std::hypot(end.x - start.x, end.y - start.y);
}

double PointLineDistanceSquared(Point const& point, Point const& line_start, Point const& line_end) {
	double dx = line_end.x - line_start.x;
	double dy = line_end.y - line_start.y;
	double denominator = dx * dx + dy * dy;
	if (denominator <= kPointEpsilon)
		return DistanceSquared(point, line_start);

	double cross = (point.x - line_start.x) * dy - (point.y - line_start.y) * dx;
	return cross * cross / denominator;
}

double SimpsonIntegral(CubicBezier const& cubic, double start, double end) {
	auto speed = [&cubic](double t) {
		Point derivative = CubicDerivativeAt(cubic, t);
		return std::hypot(derivative.x, derivative.y);
	};

	double middle = (start + end) * 0.5;
	return (end - start) * (speed(start) + 4.0 * speed(middle) + speed(end)) / 6.0;
}

double AdaptiveSimpson(CubicBezier const& cubic, double start, double end, double epsilon, double whole, int depth) {
	double middle = (start + end) * 0.5;
	double left = SimpsonIntegral(cubic, start, middle);
	double right = SimpsonIntegral(cubic, middle, end);
	double delta = left + right - whole;

	if (depth <= 0 || std::abs(delta) <= 15.0 * epsilon)
		return left + right + delta / 15.0;

	return AdaptiveSimpson(cubic, start, middle, epsilon * 0.5, left, depth - 1) +
		AdaptiveSimpson(cubic, middle, end, epsilon * 0.5, right, depth - 1);
}

double CubicLength(CubicBezier const& cubic, double end_t = 1.0) {
	double clamped_t = std::clamp(end_t, 0.0, 1.0);
	if (clamped_t <= 0.0)
		return 0.0;

	double whole = SimpsonIntegral(cubic, 0.0, clamped_t);
	return AdaptiveSimpson(cubic, 0.0, clamped_t, kLengthIntegrationEpsilon, whole, kLengthIntegrationMaxDepth);
}

double CubicTAtLength(CubicBezier const& cubic, double target_length, double total_length) {
	if (target_length <= 0.0)
		return 0.0;
	if (target_length >= total_length || total_length <= kLengthEpsilon)
		return 1.0;

	double low = 0.0;
	double high = 1.0;
	for (int iteration = 0; iteration < kLengthInverseIterations; ++iteration) {
		double middle = (low + high) * 0.5;
		if (CubicLength(cubic, middle) < target_length)
			low = middle;
		else
			high = middle;
	}

	return (low + high) * 0.5;
}

bool CubicIsFlatEnough(CubicBezier const& cubic, double tolerance_squared) {
	return std::max(
		PointLineDistanceSquared(cubic.p1, cubic.p0, cubic.p3),
		PointLineDistanceSquared(cubic.p2, cubic.p0, cubic.p3)) <= tolerance_squared;
}

Point Midpoint(Point const& lhs, Point const& rhs) {
	return {
		(lhs.x + rhs.x) * 0.5,
		(lhs.y + rhs.y) * 0.5,
	};
}

void SplitCubic(CubicBezier const& cubic, CubicBezier& left, CubicBezier& right) {
	Point p01 = Midpoint(cubic.p0, cubic.p1);
	Point p12 = Midpoint(cubic.p1, cubic.p2);
	Point p23 = Midpoint(cubic.p2, cubic.p3);
	Point p012 = Midpoint(p01, p12);
	Point p123 = Midpoint(p12, p23);
	Point p0123 = Midpoint(p012, p123);

	left = {cubic.p0, p01, p012, p0123};
	right = {p0123, p123, p23, cubic.p3};
}

void AppendLine(PathData& path, Point const& point) {
	if (!path.commands.empty() && path.commands.back().verb == PathVerb::LineTo && SamePoint(path.commands.back().p1, point))
		return;

	path.commands.push_back({PathVerb::LineTo, point, {}, {}});
}

void FlattenCubic(PathData& path, CubicBezier const& cubic, double tolerance_squared, int remaining_depth) {
	if (remaining_depth <= 0 || CubicIsFlatEnough(cubic, tolerance_squared)) {
		AppendLine(path, cubic.p3);
		return;
	}

	CubicBezier left;
	CubicBezier right;
	SplitCubic(cubic, left, right);
	FlattenCubic(path, left, tolerance_squared, remaining_depth - 1);
	FlattenCubic(path, right, tolerance_squared, remaining_depth - 1);
}

double SanitizeFlattenTolerance(double tolerance) {
	if (!(tolerance > 0.0) || !std::isfinite(tolerance))
		return kDefaultFlattenTolerance;

	return std::max(tolerance, kMinimumFlattenTolerance);
}

void AppendMeasuredSegment(MeasuredPath& measured, CubicBezier const& cubic, bool is_line) {
	double length = is_line ? LineLength(cubic.p0, cubic.p3) : CubicLength(cubic);
	if (length <= kLengthEpsilon || !std::isfinite(length))
		return;

	measured.segments.push_back({is_line, cubic, length});
	measured.total_length += length;
}

MeasuredPath BuildMeasuredPath(PathData const& path) {
	PathData lowered = LowerForAss(path, false);
	MeasuredPath measured;
	measured.segments.reserve(lowered.commands.size());

	bool has_current = false;
	Point current {};
	for (auto const& command : lowered.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				current = command.p1;
				has_current = true;
				break;
			case PathVerb::LineTo:
				if (!has_current) {
					current = {};
					has_current = true;
				}
				AppendMeasuredSegment(measured, {current, current, command.p1, command.p1}, true);
				current = command.p1;
				break;
			case PathVerb::CubicTo:
				if (!has_current) {
					current = {};
					has_current = true;
				}
				AppendMeasuredSegment(measured, {current, command.p1, command.p2, command.p3}, false);
				current = command.p3;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::Close:
				break;
		}
	}

	return measured;
}

Point SegmentPointAt(MeasuredSegment const& segment, double t) {
	if (segment.is_line)
		return Lerp(segment.cubic.p0, segment.cubic.p3, t);

	return CubicPointAt(segment.cubic, t);
}

Point SegmentTangentAt(MeasuredSegment const& segment, double t) {
	if (segment.is_line)
		return {
			segment.cubic.p3.x - segment.cubic.p0.x,
			segment.cubic.p3.y - segment.cubic.p0.y,
		};

	Point tangent = CubicDerivativeAt(segment.cubic, t);
	if (std::hypot(tangent.x, tangent.y) > kLengthEpsilon)
		return tangent;

	return {
		segment.cubic.p3.x - segment.cubic.p0.x,
		segment.cubic.p3.y - segment.cubic.p0.y,
	};
}

bool TryGetPositionAtDistance(MeasuredPath const& measured, double distance, Point& point, Point& tangent) {
	if (measured.segments.empty() || !(measured.total_length > kLengthEpsilon) || !std::isfinite(measured.total_length) || !std::isfinite(distance))
		return false;

	double target = std::clamp(distance, 0.0, measured.total_length);
	double consumed = 0.0;
	for (std::size_t index = 0; index < measured.segments.size(); ++index) {
		auto const& segment = measured.segments[index];
		bool is_last = index + 1 == measured.segments.size();
		if (!is_last && consumed + segment.length < target) {
			consumed += segment.length;
			continue;
		}

		double local_length = std::clamp(target - consumed, 0.0, segment.length);
		double t = segment.is_line ? local_length / segment.length : CubicTAtLength(segment.cubic, local_length, segment.length);
		point = SegmentPointAt(segment, t);
		tangent = SegmentTangentAt(segment, t);
		return true;
	}

	auto const& last = measured.segments.back();
	point = last.cubic.p3;
	tangent = SegmentTangentAt(last, 1.0);
	return true;
}

void IncludeAreaEdge(Point const& start, Point const& end, double& cross_sum, double& centroid_x_sum, double& centroid_y_sum) {
	double cross = start.x * end.y - end.x * start.y;
	cross_sum += cross;
	centroid_x_sum += (start.x + end.x) * cross;
	centroid_y_sum += (start.y + end.y) * cross;
}

void FlushReversedContour(PathData& reversed, Contour& contour) {
	if (contour.segments.empty()) {
		if (contour.has_anchor)
			reversed.commands.push_back({PathVerb::MoveTo, contour.anchor, {}, {}});
		contour = {};
		return;
	}

	reversed.commands.push_back({PathVerb::MoveTo, contour.segments.back().end, {}, {}});
	for (auto it = contour.segments.rbegin(); it != contour.segments.rend(); ++it) {
		if (it->verb == PathVerb::CubicTo)
			reversed.commands.push_back({PathVerb::CubicTo, it->c2, it->c1, it->start});
		else
			reversed.commands.push_back({PathVerb::LineTo, it->start, {}, {}});
	}

	contour = {};
}

} // namespace

bool TryGetBounds(PathData const& path, Rect& bounds) {
	PathData lowered = LowerForAss(path, false);
	BoundsAccumulator accumulator;
	bool has_current = false;
	Point current {};

	for (auto const& command : lowered.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				IncludePoint(accumulator, command.p1);
				current = command.p1;
				has_current = true;
				break;
			case PathVerb::LineTo:
				if (has_current)
					IncludePoint(accumulator, current);
				IncludePoint(accumulator, command.p1);
				current = command.p1;
				has_current = true;
				break;
			case PathVerb::CubicTo:
				if (!has_current)
					current = {};
				IncludeCubicBounds(accumulator, {current, command.p1, command.p2, command.p3});
				current = command.p3;
				has_current = true;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::Close:
				break;
		}
	}

	if (!accumulator.has_point)
		return false;

	bounds = {
		accumulator.min_x,
		accumulator.min_y,
		accumulator.max_x - accumulator.min_x,
		accumulator.max_y - accumulator.min_y,
	};
	return true;
}

PathData FlattenPath(PathData const& path, double tolerance) {
	PathData lowered = LowerForAss(path, false);
	PathData flattened;
	flattened.winding_fill = lowered.winding_fill;
	flattened.commands.reserve(lowered.commands.size());

	double sanitized_tolerance = SanitizeFlattenTolerance(tolerance);
	double tolerance_squared = sanitized_tolerance * sanitized_tolerance;
	bool has_current = false;
	Point current {};

	for (auto const& command : lowered.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				flattened.commands.push_back(command);
				current = command.p1;
				has_current = true;
				break;
			case PathVerb::LineTo:
				if (!has_current) {
					flattened.commands.push_back({PathVerb::MoveTo, {}, {}, {}});
					current = {};
					has_current = true;
				}
				AppendLine(flattened, command.p1);
				current = command.p1;
				break;
			case PathVerb::CubicTo:
				if (!has_current) {
					flattened.commands.push_back({PathVerb::MoveTo, {}, {}, {}});
					current = {};
					has_current = true;
				}
				FlattenCubic(flattened, {current, command.p1, command.p2, command.p3}, tolerance_squared, kMaxFlattenDepth);
				current = command.p3;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::Close:
				break;
		}
	}

	return flattened;
}

PathData ReversePath(PathData const& path) {
	PathData lowered = LowerForAss(path, false);
	PathData reversed;
	reversed.winding_fill = lowered.winding_fill;
	reversed.commands.reserve(lowered.commands.size());

	Contour contour;
	bool has_current = false;
	Point current {};

	for (auto const& command : lowered.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				FlushReversedContour(reversed, contour);
				contour.has_anchor = true;
				contour.anchor = command.p1;
				current = command.p1;
				has_current = true;
				break;
			case PathVerb::LineTo:
				if (!has_current) {
					contour.has_anchor = true;
					contour.anchor = {};
					current = {};
					has_current = true;
				}
				contour.segments.push_back({PathVerb::LineTo, current, {}, {}, command.p1});
				current = command.p1;
				break;
			case PathVerb::CubicTo:
				if (!has_current) {
					contour.has_anchor = true;
					contour.anchor = {};
					current = {};
					has_current = true;
				}
				contour.segments.push_back({PathVerb::CubicTo, current, command.p1, command.p2, command.p3});
				current = command.p3;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::Close:
				break;
		}
	}

	FlushReversedContour(reversed, contour);
	return reversed;
}

double PathLength(PathData const& path) {
	return BuildMeasuredPath(path).total_length;
}

double PercentAtLength(PathData const& path, double distance) {
	auto measured = BuildMeasuredPath(path);
	double total = measured.total_length;
	if (!(total > kLengthEpsilon) || !std::isfinite(distance))
		return 0.0;
	if (distance <= 0.0)
		return 0.0;
	if (distance >= total)
		return 1.0;

	return distance / total;
}

bool TryGetPositionAtPercent(PathData const& path, double percent, Point& point, Point& tangent) {
	if (!std::isfinite(percent))
		return false;

	auto measured = BuildMeasuredPath(path);
	return TryGetPositionAtDistance(measured, std::clamp(percent, 0.0, 1.0) * measured.total_length, point, tangent);
}

bool TryGetPositionAtLength(PathData const& path, double distance, Point& point, Point& tangent) {
	return TryGetPositionAtDistance(BuildMeasuredPath(path), distance, point, tangent);
}

bool TryGetSignedAreaAndCentroid(PathData const& path, double& signed_area, Point& centroid, double tolerance) {
	PathData flattened = FlattenPath(path, tolerance);
	double cross_sum = 0.0;
	double centroid_x_sum = 0.0;
	double centroid_y_sum = 0.0;

	bool has_current = false;
	bool contour_has_edge = false;
	Point contour_start {};
	Point current {};

	auto close_contour = [&] {
		if (has_current && contour_has_edge && !SamePoint(current, contour_start))
			IncludeAreaEdge(current, contour_start, cross_sum, centroid_x_sum, centroid_y_sum);
		contour_has_edge = false;
	};

	for (auto const& command : flattened.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				close_contour();
				contour_start = command.p1;
				current = command.p1;
				has_current = true;
				break;
			case PathVerb::LineTo:
				if (!has_current) {
					contour_start = {};
					current = {};
					has_current = true;
				}
				IncludeAreaEdge(current, command.p1, cross_sum, centroid_x_sum, centroid_y_sum);
				current = command.p1;
				contour_has_edge = true;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::CubicTo:
			case PathVerb::Close:
				break;
		}
	}
	close_contour();

	if (std::abs(cross_sum) <= kLengthEpsilon || !std::isfinite(cross_sum))
		return false;

	signed_area = cross_sum * 0.5;
	centroid = {
		centroid_x_sum / (3.0 * cross_sum),
		centroid_y_sum / (3.0 * cross_sum),
	};
	return PointIsFinite(centroid);
}

} // namespace drawing
} // namespace ass
} // namespace agi
