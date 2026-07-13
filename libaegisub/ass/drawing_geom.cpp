#include "libaegisub/ass/drawing.h"

#include <algorithm>
#include <cmath>
#include <iterator>
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
constexpr int kLengthLutMinimumDepth = 5;
constexpr int kLengthInverseIterations = 16;

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

struct RationalConic {
	Point p0 {};
	Point p1 {};
	Point p2 {};
	double weight = 1.0;
};

struct HomogeneousPoint {
	double x = 0.0;
	double y = 0.0;
	double weight = 1.0;
};

struct HomogeneousConic {
	HomogeneousPoint p0 {};
	HomogeneousPoint p1 {};
	HomogeneousPoint p2 {};
};

struct Segment {
	PathVerb verb = PathVerb::LineTo;
	Point start {};
	Point c1 {};
	Point c2 {};
	Point end {};
	double weight = 1.0;
};

struct Contour {
	bool has_anchor = false;
	bool preserve_empty_anchor = false;
	bool closed = false;
	Point anchor {};
	std::vector<Segment> segments;
};

enum class MeasuredSegmentKind {
	Line,
	Cubic,
	Conic,
};

struct ArcLengthSample {
	double t = 0.0;
	double length = 0.0;
};

struct MeasuredSegment {
	MeasuredSegmentKind kind = MeasuredSegmentKind::Line;
	CubicBezier cubic {};
	RationalConic conic {};
	double length = 0.0;
	double end_length = 0.0;
	std::vector<ArcLengthSample> arc_length_lut;
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

CubicBezier QuadAsCubic(Point const& start, Point const& control, Point const& end) {
	return {
		start,
		{
			start.x + (control.x - start.x) * (2.0 / 3.0),
			start.y + (control.y - start.y) * (2.0 / 3.0),
		},
		{
			end.x + (control.x - end.x) * (2.0 / 3.0),
			end.y + (control.y - end.y) * (2.0 / 3.0),
		},
		end,
	};
}

bool ConicIsValid(RationalConic const& conic) {
	return PointIsFinite(conic.p0) && PointIsFinite(conic.p1) && PointIsFinite(conic.p2) &&
		conic.weight > 0.0 && std::isfinite(conic.weight);
}

Point ConicPointAt(RationalConic const& conic, double t) {
	double mt = 1.0 - t;
	double a = mt * mt;
	double b = 2.0 * conic.weight * t * mt;
	double c = t * t;
	double denominator = a + b + c;
	if (!(denominator > 0.0) || !std::isfinite(denominator))
		return conic.p2;

	return {
		(a * conic.p0.x + b * conic.p1.x + c * conic.p2.x) / denominator,
		(a * conic.p0.y + b * conic.p1.y + c * conic.p2.y) / denominator,
	};
}

Point ConicDerivativeAt(RationalConic const& conic, double t) {
	double mt = 1.0 - t;
	double a = mt * mt;
	double b = 2.0 * conic.weight * t * mt;
	double c = t * t;
	double da = -2.0 * mt;
	double db = 2.0 * conic.weight * (1.0 - 2.0 * t);
	double dc = 2.0 * t;
	double denominator = a + b + c;
	if (!(denominator > 0.0) || !std::isfinite(denominator))
		return {};

	Point numerator {
		a * conic.p0.x + b * conic.p1.x + c * conic.p2.x,
		a * conic.p0.y + b * conic.p1.y + c * conic.p2.y,
	};
	Point numerator_derivative {
		da * conic.p0.x + db * conic.p1.x + dc * conic.p2.x,
		da * conic.p0.y + db * conic.p1.y + dc * conic.p2.y,
	};
	double inverse_denominator_squared = 1.0 / (denominator * denominator);
	return {
		(numerator_derivative.x * denominator - numerator.x * (da + db + dc)) * inverse_denominator_squared,
		(numerator_derivative.y * denominator - numerator.y * (da + db + dc)) * inverse_denominator_squared,
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

void IncludeConicRoot(BoundsAccumulator& bounds, RationalConic const& conic, double t) {
	if (t > 0.0 && t < 1.0 && std::isfinite(t))
		IncludePoint(bounds, ConicPointAt(conic, t));
}

void IncludeConicCoordinateExtrema(BoundsAccumulator& bounds,
	RationalConic const& conic,
	double p0,
	double p1,
	double p2) {
	// N(t) / D(t), where N and D are quadratics.  The cubic terms
	// cancel in N'D - ND', leaving this quadratic derivative numerator.
	double numerator_a = p0 - 2.0 * conic.weight * p1 + p2;
	double numerator_b = -2.0 * p0 + 2.0 * conic.weight * p1;
	double numerator_c = p0;
	double denominator_a = 2.0 * (1.0 - conic.weight);
	double denominator_b = 2.0 * (conic.weight - 1.0);
	double derivative_a = numerator_a * denominator_b - numerator_b * denominator_a;
	double derivative_b = 2.0 * (numerator_a - numerator_c * denominator_a);
	double derivative_c = numerator_b - numerator_c * denominator_b;

	if (!std::isfinite(derivative_a) || !std::isfinite(derivative_b) || !std::isfinite(derivative_c))
		return;

	double scale = std::max({1.0, std::abs(derivative_a), std::abs(derivative_b), std::abs(derivative_c)});
	double epsilon = std::numeric_limits<double>::epsilon() * scale * 16.0;
	if (std::abs(derivative_a) <= epsilon) {
		if (std::abs(derivative_b) > epsilon)
			IncludeConicRoot(bounds, conic, -derivative_c / derivative_b);
		return;
	}

	double discriminant = derivative_b * derivative_b - 4.0 * derivative_a * derivative_c;
	double discriminant_epsilon = std::numeric_limits<double>::epsilon() *
		(std::abs(derivative_b * derivative_b) + std::abs(4.0 * derivative_a * derivative_c)) * 16.0;
	if (discriminant < -discriminant_epsilon)
		return;

	double root = std::sqrt(std::max(0.0, discriminant));
	double q = -0.5 * (derivative_b + std::copysign(root, derivative_b));
	if (std::abs(q) <= epsilon) {
		IncludeConicRoot(bounds, conic, -derivative_b / (2.0 * derivative_a));
		return;
	}

	IncludeConicRoot(bounds, conic, q / derivative_a);
	IncludeConicRoot(bounds, conic, derivative_c / q);
}

void IncludeConicBounds(BoundsAccumulator& bounds, RationalConic const& conic) {
	IncludePoint(bounds, conic.p0);
	IncludePoint(bounds, conic.p2);
	IncludeConicCoordinateExtrema(bounds, conic, conic.p0.x, conic.p1.x, conic.p2.x);
	IncludeConicCoordinateExtrema(bounds, conic, conic.p0.y, conic.p1.y, conic.p2.y);
}

double DistanceSquared(Point const& lhs, Point const& rhs) {
	double dx = lhs.x - rhs.x;
	double dy = lhs.y - rhs.y;
	return dx * dx + dy * dy;
}

double LineLength(Point const& start, Point const& end) {
	return std::hypot(end.x - start.x, end.y - start.y);
}

double PointSegmentDistanceSquared(Point const& point, Point const& segment_start, Point const& segment_end) {
	double dx = segment_end.x - segment_start.x;
	double dy = segment_end.y - segment_start.y;
	double denominator = dx * dx + dy * dy;
	if (denominator <= kPointEpsilon)
		return DistanceSquared(point, segment_start);

	double projection = ((point.x - segment_start.x) * dx + (point.y - segment_start.y) * dy) / denominator;
	return DistanceSquared(point, Lerp(segment_start, segment_end, std::clamp(projection, 0.0, 1.0)));
}

double SimpsonIntegral(CubicBezier const& cubic, double start, double end) {
	auto speed = [&cubic](double t) {
		Point derivative = CubicDerivativeAt(cubic, t);
		return std::hypot(derivative.x, derivative.y);
	};

	double middle = (start + end) * 0.5;
	return (end - start) * (speed(start) + 4.0 * speed(middle) + speed(end)) / 6.0;
}

template<typename Simpson>
double AdaptiveIntegral(Simpson const& simpson, double start, double end, double epsilon, double whole, int depth) {
	double middle = (start + end) * 0.5;
	double left = simpson(start, middle);
	double right = simpson(middle, end);
	double delta = left + right - whole;

	if (depth <= 0 || std::abs(delta) <= 15.0 * epsilon)
		return left + right + delta / 15.0;

	return AdaptiveIntegral(simpson, start, middle, epsilon * 0.5, left, depth - 1) +
		AdaptiveIntegral(simpson, middle, end, epsilon * 0.5, right, depth - 1);
}

template<typename Simpson>
double IntegrateInterval(Simpson const& simpson, double start, double end) {
	if (!(end > start))
		return 0.0;

	double whole = simpson(start, end);
	return AdaptiveIntegral(simpson, start, end,
		kLengthIntegrationEpsilon, whole, kLengthIntegrationMaxDepth);
}

template<typename Simpson>
void AppendArcLengthSamples(Simpson const& simpson,
	double start,
	double end,
	double epsilon,
	double whole,
	int depth,
	int minimum_depth,
	double& cumulative_length,
	std::vector<ArcLengthSample>& samples) {
	double middle = (start + end) * 0.5;
	double left = simpson(start, middle);
	double right = simpson(middle, end);
	double delta = left + right - whole;

	if (depth <= 0 || (minimum_depth <= 0 && std::abs(delta) <= 15.0 * epsilon)) {
		cumulative_length += left + right + delta / 15.0;
		samples.push_back({end, cumulative_length});
		return;
	}

	AppendArcLengthSamples(simpson, start, middle, epsilon * 0.5, left,
		depth - 1, minimum_depth - 1, cumulative_length, samples);
	AppendArcLengthSamples(simpson, middle, end, epsilon * 0.5, right,
		depth - 1, minimum_depth - 1, cumulative_length, samples);
}

template<typename Simpson>
std::vector<ArcLengthSample> BuildArcLengthLut(Simpson const& simpson) {
	std::vector<ArcLengthSample> samples;
	samples.reserve((1 << kLengthLutMinimumDepth) + 1);
	samples.push_back({0.0, 0.0});
	double cumulative_length = 0.0;
	double whole = simpson(0.0, 1.0);
	AppendArcLengthSamples(simpson, 0.0, 1.0, kLengthIntegrationEpsilon, whole,
		kLengthIntegrationMaxDepth, kLengthLutMinimumDepth, cumulative_length, samples);
	return samples;
}

double ConicSimpsonIntegral(RationalConic const& conic, double start, double end) {
	auto speed = [&conic](double t) {
		Point derivative = ConicDerivativeAt(conic, t);
		return std::hypot(derivative.x, derivative.y);
	};

	double middle = (start + end) * 0.5;
	return (end - start) * (speed(start) + 4.0 * speed(middle) + speed(end)) / 6.0;
}

template<typename Simpson, typename Speed>
double CurveTAtLength(std::vector<ArcLengthSample> const& lut,
	double target_length,
	double total_length,
	Simpson const& simpson,
	Speed const& speed) {
	if (target_length <= 0.0)
		return 0.0;
	if (target_length >= total_length || total_length <= kLengthEpsilon || lut.size() < 2)
		return 1.0;

	auto upper = std::lower_bound(lut.begin() + 1, lut.end(), target_length,
		[](ArcLengthSample const& sample, double length) { return sample.length < length; });
	if (upper == lut.end())
		return 1.0;
	auto const& anchor = *std::prev(upper);

	double low = anchor.t;
	double high = upper->t;
	double interval_length = upper->length - anchor.length;
	double t = interval_length > kLengthEpsilon
		? low + (high - low) * (target_length - anchor.length) / interval_length
		: (low + high) * 0.5;

	for (int iteration = 0; iteration < kLengthInverseIterations; ++iteration) {
		double current_length = anchor.length + IntegrateInterval(simpson, anchor.t, t);
		double error = current_length - target_length;
		if (std::abs(error) <= kLengthIntegrationEpsilon * 0.25)
			return t;

		if (error < 0.0)
			low = t;
		else
			high = t;

		double candidate = t;
		double current_speed = speed(t);
		if (current_speed > kLengthEpsilon && std::isfinite(current_speed))
			candidate -= error / current_speed;

		double margin = (high - low) * 0.05;
		if (!(candidate > low + margin && candidate < high - margin) || !std::isfinite(candidate))
			candidate = (low + high) * 0.5;
		t = candidate;
	}

	return (low + high) * 0.5;
}

std::vector<ArcLengthSample> BuildCubicArcLengthLut(CubicBezier const& cubic) {
	return BuildArcLengthLut([&](double start, double end) {
		return SimpsonIntegral(cubic, start, end);
	});
}

std::vector<ArcLengthSample> BuildConicArcLengthLut(RationalConic const& conic) {
	return BuildArcLengthLut([&](double start, double end) {
		return ConicSimpsonIntegral(conic, start, end);
	});
}

double CubicTAtLength(CubicBezier const& cubic,
	std::vector<ArcLengthSample> const& lut,
	double target_length,
	double total_length) {
	return CurveTAtLength(lut, target_length, total_length,
		[&](double start, double end) { return SimpsonIntegral(cubic, start, end); },
		[&](double t) {
			Point derivative = CubicDerivativeAt(cubic, t);
			return std::hypot(derivative.x, derivative.y);
		});
}

double ConicTAtLength(RationalConic const& conic,
	std::vector<ArcLengthSample> const& lut,
	double target_length,
	double total_length) {
	return CurveTAtLength(lut, target_length, total_length,
		[&](double start, double end) { return ConicSimpsonIntegral(conic, start, end); },
		[&](double t) {
			Point derivative = ConicDerivativeAt(conic, t);
			return std::hypot(derivative.x, derivative.y);
		});
}

bool CubicIsFlatEnough(CubicBezier const& cubic, double tolerance_squared) {
	return std::max(
		PointSegmentDistanceSquared(cubic.p1, cubic.p0, cubic.p3),
		PointSegmentDistanceSquared(cubic.p2, cubic.p0, cubic.p3)) <= tolerance_squared;
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

HomogeneousPoint HomogeneousMidpoint(HomogeneousPoint const& lhs, HomogeneousPoint const& rhs) {
	return {
		(lhs.x + rhs.x) * 0.5,
		(lhs.y + rhs.y) * 0.5,
		(lhs.weight + rhs.weight) * 0.5,
	};
}

Point Project(HomogeneousPoint const& point) {
	if (!(point.weight > 0.0) || !std::isfinite(point.weight))
		return {};
	return {point.x / point.weight, point.y / point.weight};
}

bool MakeHomogeneousConic(RationalConic const& conic, HomogeneousConic& homogeneous) {
	if (!ConicIsValid(conic))
		return false;

	homogeneous = {
		{conic.p0.x, conic.p0.y, 1.0},
		{conic.p1.x * conic.weight, conic.p1.y * conic.weight, conic.weight},
		{conic.p2.x, conic.p2.y, 1.0},
	};
	return std::isfinite(homogeneous.p1.x) && std::isfinite(homogeneous.p1.y);
}

void SplitConic(HomogeneousConic const& conic, HomogeneousConic& left, HomogeneousConic& right) {
	HomogeneousPoint p01 = HomogeneousMidpoint(conic.p0, conic.p1);
	HomogeneousPoint p12 = HomogeneousMidpoint(conic.p1, conic.p2);
	HomogeneousPoint p012 = HomogeneousMidpoint(p01, p12);
	left = {conic.p0, p01, p012};
	right = {p012, p12, conic.p2};
}

bool ConicIsFlatEnough(HomogeneousConic const& conic, double tolerance_squared) {
	Point start = Project(conic.p0);
	Point control = Project(conic.p1);
	Point end = Project(conic.p2);
	return PointSegmentDistanceSquared(control, start, end) <= tolerance_squared;
}

void FlattenConic(PathData& path, HomogeneousConic const& conic, double tolerance_squared, int remaining_depth) {
	if (remaining_depth <= 0 || ConicIsFlatEnough(conic, tolerance_squared)) {
		AppendLine(path, Project(conic.p2));
		return;
	}

	HomogeneousConic left;
	HomogeneousConic right;
	SplitConic(conic, left, right);
	FlattenConic(path, left, tolerance_squared, remaining_depth - 1);
	FlattenConic(path, right, tolerance_squared, remaining_depth - 1);
}

double SanitizeFlattenTolerance(double tolerance) {
	if (!(tolerance > 0.0) || !std::isfinite(tolerance))
		return kDefaultFlattenTolerance;

	return std::max(tolerance, kMinimumFlattenTolerance);
}

void AppendMeasuredLine(MeasuredPath& measured, Point const& start, Point const& end) {
	double length = LineLength(start, end);
	if (length <= kLengthEpsilon || !std::isfinite(length))
		return;

	MeasuredSegment segment;
	segment.kind = MeasuredSegmentKind::Line;
	segment.cubic = {start, start, end, end};
	segment.length = length;
	measured.total_length += length;
	segment.end_length = measured.total_length;
	measured.segments.push_back(segment);
}

void AppendMeasuredCubic(MeasuredPath& measured, CubicBezier const& cubic) {
	auto arc_length_lut = BuildCubicArcLengthLut(cubic);
	double length = arc_length_lut.back().length;
	if (length <= kLengthEpsilon || !std::isfinite(length))
		return;

	MeasuredSegment segment;
	segment.kind = MeasuredSegmentKind::Cubic;
	segment.cubic = cubic;
	segment.length = length;
	segment.arc_length_lut = std::move(arc_length_lut);
	measured.total_length += length;
	segment.end_length = measured.total_length;
	measured.segments.push_back(std::move(segment));
}

void AppendMeasuredConic(MeasuredPath& measured, RationalConic const& conic) {
	if (!ConicIsValid(conic)) {
		AppendMeasuredLine(measured, conic.p0, conic.p2);
		return;
	}

	auto arc_length_lut = BuildConicArcLengthLut(conic);
	double length = arc_length_lut.back().length;
	if (length <= kLengthEpsilon || !std::isfinite(length)) {
		AppendMeasuredLine(measured, conic.p0, conic.p2);
		return;
	}

	MeasuredSegment segment;
	segment.kind = MeasuredSegmentKind::Conic;
	segment.conic = conic;
	segment.length = length;
	segment.arc_length_lut = std::move(arc_length_lut);
	measured.total_length += length;
	segment.end_length = measured.total_length;
	measured.segments.push_back(std::move(segment));
}

void EnsureRawCurrent(bool& has_current,
	Point& current,
	bool& has_contour_start,
	Point& contour_start) {
	if (has_current)
		return;

	has_current = true;
	current = {};
	has_contour_start = true;
	contour_start = {};
}

MeasuredPath BuildMeasuredPath(PathData const& path) {
	MeasuredPath measured;
	measured.segments.reserve(path.commands.size());

	bool has_current = false;
	bool has_contour_start = false;
	Point contour_start {};
	Point current {};
	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				current = command.p1;
				has_current = true;
				contour_start = command.p1;
				has_contour_start = true;
				break;
			case PathVerb::LineTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				AppendMeasuredLine(measured, current, command.p1);
				current = command.p1;
				break;
			case PathVerb::QuadTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				AppendMeasuredCubic(measured, QuadAsCubic(current, command.p1, command.p2));
				current = command.p2;
				break;
			case PathVerb::ConicTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				AppendMeasuredConic(measured, {current, command.p1, command.p2, command.weight});
				current = command.p2;
				break;
			case PathVerb::CubicTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				AppendMeasuredCubic(measured, {current, command.p1, command.p2, command.p3});
				current = command.p3;
				break;
			case PathVerb::Close:
				if (has_current && has_contour_start) {
					AppendMeasuredLine(measured, current, contour_start);
					current = contour_start;
				}
				break;
		}
	}

	return measured;
}

Point SegmentPointAt(MeasuredSegment const& segment, double t) {
	switch (segment.kind) {
		case MeasuredSegmentKind::Line:
			return Lerp(segment.cubic.p0, segment.cubic.p3, t);
		case MeasuredSegmentKind::Cubic:
			return CubicPointAt(segment.cubic, t);
		case MeasuredSegmentKind::Conic:
			return ConicPointAt(segment.conic, t);
	}
	return {};
}

Point SegmentTangentAt(MeasuredSegment const& segment, double t) {
	if (segment.kind == MeasuredSegmentKind::Line)
		return {
			segment.cubic.p3.x - segment.cubic.p0.x,
			segment.cubic.p3.y - segment.cubic.p0.y,
		};

	Point tangent = segment.kind == MeasuredSegmentKind::Cubic ?
		CubicDerivativeAt(segment.cubic, t) : ConicDerivativeAt(segment.conic, t);
	if (std::hypot(tangent.x, tangent.y) > kLengthEpsilon)
		return tangent;

	Point start = segment.kind == MeasuredSegmentKind::Cubic ? segment.cubic.p0 : segment.conic.p0;
	Point end = segment.kind == MeasuredSegmentKind::Cubic ? segment.cubic.p3 : segment.conic.p2;
	return {
		end.x - start.x,
		end.y - start.y,
	};
}

double SegmentTAtLength(MeasuredSegment const& segment, double local_length) {
	switch (segment.kind) {
		case MeasuredSegmentKind::Line:
			return local_length / segment.length;
		case MeasuredSegmentKind::Cubic:
			return CubicTAtLength(segment.cubic, segment.arc_length_lut, local_length, segment.length);
		case MeasuredSegmentKind::Conic:
			return ConicTAtLength(segment.conic, segment.arc_length_lut, local_length, segment.length);
	}
	return 0.0;
}

Point SegmentEnd(MeasuredSegment const& segment) {
	return segment.kind == MeasuredSegmentKind::Conic ? segment.conic.p2 : segment.cubic.p3;
}

MeasuredSegment const *FindMeasuredSegment(MeasuredPath const& measured, double target, double& consumed) {
	auto it = std::lower_bound(measured.segments.begin(), measured.segments.end(), target,
		[](MeasuredSegment const& segment, double distance) { return segment.end_length < distance; });
	if (it == measured.segments.end())
		return nullptr;

	consumed = it == measured.segments.begin() ? 0.0 : std::prev(it)->end_length;
	return &*it;
}

bool TryGetPositionAtDistance(MeasuredPath const& measured, double distance, Point& point, Point& tangent) {
	if (measured.segments.empty() || !(measured.total_length > kLengthEpsilon) || !std::isfinite(measured.total_length) || !std::isfinite(distance))
		return false;

	double target = std::clamp(distance, 0.0, measured.total_length);
	double consumed = 0.0;
	if (auto const *segment = FindMeasuredSegment(measured, target, consumed)) {
		double local_length = std::clamp(target - consumed, 0.0, segment->length);
		double t = SegmentTAtLength(*segment, local_length);
		point = SegmentPointAt(*segment, t);
		tangent = SegmentTangentAt(*segment, t);
		return true;
	}

	auto const& last = measured.segments.back();
	point = SegmentEnd(last);
	tangent = SegmentTangentAt(last, 1.0);
	return true;
}

bool TryGetPositionAtLegacyPercent(MeasuredPath const& measured, double percent, Point& point, Point& tangent) {
	if (measured.segments.empty() || !(measured.total_length > kLengthEpsilon) || !std::isfinite(measured.total_length))
		return false;

	double target = percent * measured.total_length;
	double consumed = 0.0;
	if (auto const *segment = FindMeasuredSegment(measured, target, consumed)) {
		// QPainterPath allocates the global percentage by measured segment
		// length, but uses that segment-local allocation directly as the
		// Bezier parameter rather than as a local arc-length percentage.
		double t = std::clamp((target - consumed) / segment->length, 0.0, 1.0);
		point = SegmentPointAt(*segment, t);
		tangent = SegmentTangentAt(*segment, t);
		return true;
	}

	return false;
}

bool TryGetDegeneratePathPosition(PathData const& path, Point& point) {
	bool has_current = false;
	bool has_contour_start = false;
	Point current {};
	Point contour_start {};
	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				current = command.p1;
				contour_start = command.p1;
				has_current = true;
				has_contour_start = true;
				break;
			case PathVerb::LineTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				current = command.p1;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				current = command.p2;
				break;
			case PathVerb::CubicTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				current = command.p3;
				break;
			case PathVerb::Close:
				if (has_current && has_contour_start)
					current = contour_start;
				break;
		}
	}

	if (!has_current || !PointIsFinite(current))
		return false;
	point = current;
	return true;
}

void IncludeAreaEdge(Point const& start,
	Point const& end,
	Point const& origin,
	double& cross_sum,
	double& centroid_x_sum,
	double& centroid_y_sum) {
	Point local_start {start.x - origin.x, start.y - origin.y};
	Point local_end {end.x - origin.x, end.y - origin.y};
	double cross = local_start.x * local_end.y - local_end.x * local_start.y;
	cross_sum += cross;
	centroid_x_sum += (local_start.x + local_end.x) * cross;
	centroid_y_sum += (local_start.y + local_end.y) * cross;
}

void FlushReversedContour(PathData& reversed, Contour& contour) {
	if (contour.segments.empty()) {
		if (contour.has_anchor && contour.preserve_empty_anchor) {
			reversed.commands.push_back({PathVerb::MoveTo, contour.anchor, {}, {}});
			if (contour.closed)
				reversed.commands.push_back({PathVerb::Close, {}, {}, {}});
		}
		contour = {};
		return;
	}

	reversed.commands.push_back({PathVerb::MoveTo, contour.segments.back().end, {}, {}});
	for (auto it = contour.segments.rbegin(); it != contour.segments.rend(); ++it) {
		switch (it->verb) {
			case PathVerb::LineTo:
				reversed.commands.push_back({PathVerb::LineTo, it->start, {}, {}});
				break;
			case PathVerb::QuadTo:
				reversed.commands.push_back({PathVerb::QuadTo, it->c1, it->start, {}});
				break;
			case PathVerb::ConicTo:
				reversed.commands.push_back({PathVerb::ConicTo, it->c1, it->start, {}, it->weight});
				break;
			case PathVerb::CubicTo:
				reversed.commands.push_back({PathVerb::CubicTo, it->c2, it->c1, it->start});
				break;
			case PathVerb::MoveTo:
			case PathVerb::Close:
				break;
		}
	}
	if (contour.closed)
		reversed.commands.push_back({PathVerb::Close, {}, {}, {}});

	contour = {};
}

} // namespace

bool TryGetBounds(PathData const& path, Rect& bounds) {
	BoundsAccumulator accumulator;
	bool has_current = false;
	bool has_contour_start = false;
	Point contour_start {};
	Point current {};

	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				IncludePoint(accumulator, command.p1);
				current = command.p1;
				has_current = true;
				contour_start = command.p1;
				has_contour_start = true;
				break;
			case PathVerb::LineTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				IncludePoint(accumulator, current);
				IncludePoint(accumulator, command.p1);
				current = command.p1;
				break;
			case PathVerb::QuadTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				IncludeCubicBounds(accumulator, QuadAsCubic(current, command.p1, command.p2));
				current = command.p2;
				break;
			case PathVerb::ConicTo: {
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				RationalConic conic {current, command.p1, command.p2, command.weight};
				if (ConicIsValid(conic))
					IncludeConicBounds(accumulator, conic);
				else {
					// Invalid/non-positive rational weights have no stable conic
					// interpretation; match the other geometry queries by using
					// a line to the declared endpoint.
					IncludePoint(accumulator, current);
					IncludePoint(accumulator, command.p2);
				}
				current = command.p2;
				break;
			}
			case PathVerb::CubicTo:
				EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
				IncludeCubicBounds(accumulator, {current, command.p1, command.p2, command.p3});
				current = command.p3;
				break;
			case PathVerb::Close:
				if (has_current && has_contour_start) {
					IncludePoint(accumulator, current);
					IncludePoint(accumulator, contour_start);
					current = contour_start;
				}
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

bool TryGetControlPointBounds(PathData const& path, Rect& bounds) {
	// Legacy controlPointRect semantics visit only stored command points.
	// Curve extrema are intentionally ignored for shape_bouding* compatibility.
	BoundsAccumulator accumulator;

	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
			case PathVerb::LineTo:
				IncludePoint(accumulator, command.p1);
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
				IncludePoint(accumulator, command.p1);
				IncludePoint(accumulator, command.p2);
				break;
			case PathVerb::CubicTo:
				IncludePoint(accumulator, command.p1);
				IncludePoint(accumulator, command.p2);
				IncludePoint(accumulator, command.p3);
				break;
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
	PathData flattened;
	flattened.winding_fill = path.winding_fill;
	flattened.commands.reserve(path.commands.size());

	double sanitized_tolerance = SanitizeFlattenTolerance(tolerance);
	double tolerance_squared = sanitized_tolerance * sanitized_tolerance;
	bool has_current = false;
	bool has_contour_start = false;
	Point contour_start {};
	Point current {};

	auto ensure_output_current = [&] {
		if (has_current)
			return;
		flattened.commands.push_back({PathVerb::MoveTo, {}, {}, {}});
		EnsureRawCurrent(has_current, current, has_contour_start, contour_start);
	};

	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				flattened.commands.push_back(command);
				current = command.p1;
				has_current = true;
				contour_start = command.p1;
				has_contour_start = true;
				break;
			case PathVerb::LineTo:
				ensure_output_current();
				AppendLine(flattened, command.p1);
				current = command.p1;
				break;
			case PathVerb::QuadTo:
				ensure_output_current();
				FlattenCubic(flattened,
					QuadAsCubic(current, command.p1, command.p2),
					tolerance_squared,
					kMaxFlattenDepth);
				current = command.p2;
				break;
			case PathVerb::ConicTo: {
				ensure_output_current();
				RationalConic conic {current, command.p1, command.p2, command.weight};
				HomogeneousConic homogeneous;
				if (MakeHomogeneousConic(conic, homogeneous))
					FlattenConic(flattened, homogeneous, tolerance_squared, kMaxFlattenDepth);
				else
					AppendLine(flattened, command.p2);
				current = command.p2;
				break;
			}
			case PathVerb::CubicTo:
				ensure_output_current();
				FlattenCubic(flattened, {current, command.p1, command.p2, command.p3}, tolerance_squared, kMaxFlattenDepth);
				current = command.p3;
				break;
			case PathVerb::Close:
				flattened.commands.push_back(command);
				if (has_current && has_contour_start)
					current = contour_start;
				break;
		}
	}

	return flattened;
}

PathData ReversePath(PathData const& path) {
	PathData reversed;
	reversed.winding_fill = path.winding_fill;
	reversed.commands.reserve(path.commands.size());

	Contour contour;
	bool has_current = false;
	bool has_contour_start = false;
	Point contour_start {};
	Point current {};

	auto ensure_contour = [&] {
		if (has_current)
			return;
		has_current = true;
		current = {};
		has_contour_start = true;
		contour_start = {};
		contour.has_anchor = true;
		contour.anchor = {};
	};

	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				FlushReversedContour(reversed, contour);
				contour.has_anchor = true;
				contour.preserve_empty_anchor = true;
				contour.anchor = command.p1;
				current = command.p1;
				has_current = true;
				contour_start = command.p1;
				has_contour_start = true;
				break;
			case PathVerb::LineTo:
				ensure_contour();
				contour.segments.push_back({PathVerb::LineTo, current, {}, {}, command.p1});
				current = command.p1;
				break;
			case PathVerb::QuadTo:
				ensure_contour();
				contour.segments.push_back({PathVerb::QuadTo, current, command.p1, {}, command.p2});
				current = command.p2;
				break;
			case PathVerb::ConicTo:
				ensure_contour();
				contour.segments.push_back({PathVerb::ConicTo, current, command.p1, {}, command.p2, command.weight});
				current = command.p2;
				break;
			case PathVerb::CubicTo:
				ensure_contour();
				contour.segments.push_back({PathVerb::CubicTo, current, command.p1, command.p2, command.p3});
				current = command.p3;
				break;
			case PathVerb::Close: {
				if (!has_current || !has_contour_start) {
					FlushReversedContour(reversed, contour);
					reversed.commands.push_back(command);
					break;
				}

				Point closed_start = contour_start;
				contour.closed = true;
				FlushReversedContour(reversed, contour);

				// A command following Close continues at the just-closed
				// contour's start.  Keep that point as an implicit anchor,
				// but do not emit an extra move when no command follows.
				has_current = true;
				current = closed_start;
				has_contour_start = true;
				contour_start = closed_start;
				contour.has_anchor = true;
				contour.anchor = closed_start;
				break;
			}
		}
	}

	FlushReversedContour(reversed, contour);
	return reversed;
}

struct PathMeasure::Impl {
	MeasuredPath measured;
	bool has_degenerate_position = false;
	Point degenerate_position {};
};

PathMeasure::PathMeasure(PathData const& path) {
	auto impl = std::make_shared<Impl>();
	impl->measured = BuildMeasuredPath(path);
	impl->has_degenerate_position = TryGetDegeneratePathPosition(path, impl->degenerate_position);
	impl_ = std::move(impl);
}

double PathMeasure::Length() const {
	return impl_->measured.total_length;
}

double PathMeasure::PercentAtLength(double distance) const {
	double total = impl_->measured.total_length;
	if (!(total > kLengthEpsilon) || !std::isfinite(distance))
		return 0.0;
	if (distance <= 0.0)
		return 0.0;
	if (distance >= total)
		return 1.0;

	return distance / total;
}

double PathMeasure::LegacyPercentAtLength(double distance) const {
	auto const& measured = impl_->measured;
	double total = measured.total_length;
	if (!(total > kLengthEpsilon) || !std::isfinite(total) || std::isnan(distance))
		return 0.0;
	if (distance <= 0.0)
		return 0.0;
	if (distance >= total)
		return 1.0;

	double consumed = 0.0;
	if (auto const *segment = FindMeasuredSegment(measured, distance, consumed)) {
		double local_length = std::clamp(distance - consumed, 0.0, segment->length);
		double t = SegmentTAtLength(*segment, local_length);
		return std::clamp((consumed + t * segment->length) / total, 0.0, 1.0);
	}

	return 0.0;
}

bool PathMeasure::TryGetPositionAtPercent(double percent, Point& point, Point& tangent) const {
	if (!std::isfinite(percent))
		return false;

	auto const& measured = impl_->measured;
	return TryGetPositionAtDistance(measured, std::clamp(percent, 0.0, 1.0) * measured.total_length, point, tangent);
}

bool PathMeasure::TryGetLegacyPositionAtPercent(double percent, Point& point, Point& tangent) const {
	point = {};
	tangent = {};
	if (!std::isfinite(percent) || percent < 0.0 || percent > 1.0)
		return false;

	auto const& measured = impl_->measured;
	if (TryGetPositionAtLegacyPercent(measured, percent, point, tangent))
		return true;

	// QPainterPath::pointAtPercent returns its only MoveTo anchor for a
	// move-only path.  Keep that useful degenerate-path behavior while
	// angle/slope naturally remain zero through the zero tangent.
	if (!impl_->has_degenerate_position)
		return false;
	point = impl_->degenerate_position;
	return true;
}

bool PathMeasure::TryGetPositionAtLength(double distance, Point& point, Point& tangent) const {
	return TryGetPositionAtDistance(impl_->measured, distance, point, tangent);
}

double PathLength(PathData const& path) {
	return PathMeasure(path).Length();
}

double PercentAtLength(PathData const& path, double distance) {
	return PathMeasure(path).PercentAtLength(distance);
}

double LegacyPercentAtLength(PathData const& path, double distance) {
	return PathMeasure(path).LegacyPercentAtLength(distance);
}

bool TryGetPositionAtPercent(PathData const& path, double percent, Point& point, Point& tangent) {
	return PathMeasure(path).TryGetPositionAtPercent(percent, point, tangent);
}

bool TryGetLegacyPositionAtPercent(PathData const& path, double percent, Point& point, Point& tangent) {
	return PathMeasure(path).TryGetLegacyPositionAtPercent(percent, point, tangent);
}

bool TryGetPositionAtLength(PathData const& path, double distance, Point& point, Point& tangent) {
	return PathMeasure(path).TryGetPositionAtLength(distance, point, tangent);
}

bool TryGetSignedAreaAndCentroid(PathData const& path, double& signed_area, Point& centroid, double tolerance) {
	PathData flattened = FlattenPath(path, tolerance);
	double cross_sum = 0.0;
	double centroid_x_cross_sum = 0.0;
	double centroid_y_cross_sum = 0.0;

	bool has_current = false;
	bool contour_has_edge = false;
	bool has_reference = false;
	Point reference {};
	Point contour_start {};
	Point current {};
	double contour_cross_sum = 0.0;
	double contour_centroid_x_sum = 0.0;
	double contour_centroid_y_sum = 0.0;

	auto close_contour = [&] {
		if (has_current && contour_has_edge && !SamePoint(current, contour_start))
			IncludeAreaEdge(current, contour_start, contour_start,
				contour_cross_sum, contour_centroid_x_sum, contour_centroid_y_sum);
		if (contour_has_edge) {
			if (!has_reference) {
				reference = contour_start;
				has_reference = true;
			}
			cross_sum += contour_cross_sum;
			centroid_x_cross_sum += (contour_start.x - reference.x) * contour_cross_sum + contour_centroid_x_sum / 3.0;
			centroid_y_cross_sum += (contour_start.y - reference.y) * contour_cross_sum + contour_centroid_y_sum / 3.0;
		}
		contour_has_edge = false;
		contour_cross_sum = 0.0;
		contour_centroid_x_sum = 0.0;
		contour_centroid_y_sum = 0.0;
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
				IncludeAreaEdge(current, command.p1, contour_start,
					contour_cross_sum, contour_centroid_x_sum, contour_centroid_y_sum);
				current = command.p1;
				contour_has_edge = true;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::CubicTo:
				break;
			case PathVerb::Close:
				close_contour();
				if (has_current)
					current = contour_start;
				break;
		}
	}
	close_contour();

	if (std::abs(cross_sum) <= kLengthEpsilon || !std::isfinite(cross_sum))
		return false;

	signed_area = cross_sum * 0.5;
	centroid = {
		reference.x + centroid_x_cross_sum / cross_sum,
		reference.y + centroid_y_cross_sum / cross_sum,
	};
	return PointIsFinite(centroid);
}

} // namespace drawing
} // namespace ass
} // namespace agi
