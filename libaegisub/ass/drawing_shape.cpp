#include "libaegisub/ass/drawing.h"

#include <algorithm>
#include <cmath>

namespace agi {
namespace ass {
namespace drawing {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBezierCircle = 0.5522847498307936;

struct PathState {
	bool has_current = false;
	Point current {};
	bool has_subpath_start = false;
	Point subpath_start {};
};

struct CubicSegment {
	Point start {};
	Point control1 {};
	Point control2 {};
	Point end {};
};

Rect NormalizeRect(double x, double y, double width, double height) {
	if (width < 0.0) {
		x += width;
		width = -width;
	}
	if (height < 0.0) {
		y += height;
		height = -height;
	}
	return {x, y, width, height};
}

bool RectIsFinite(Rect const& rect) {
	return std::isfinite(rect.x) && std::isfinite(rect.y) &&
		std::isfinite(rect.width) && std::isfinite(rect.height) &&
		std::isfinite(rect.x + rect.width) && std::isfinite(rect.y + rect.height);
}

double DegreesToRadians(double degrees) {
	return degrees * kPi / 180.0;
}

double NormalizeDegrees(double degrees) {
	double normalized = std::fmod(degrees, 360.0);
	if (normalized < 0.0)
		normalized += 360.0;
	return normalized == 0.0 ? 0.0 : normalized;
}

Point EllipsePoint(Rect const& rect, double angle_degrees) {
	double radius_x = rect.width * 0.5;
	double radius_y = rect.height * 0.5;
	double center_x = rect.x + radius_x;
	double center_y = rect.y + radius_y;
	double angle = NormalizeDegrees(angle_degrees);

	// Keep the quadrant points exact. Besides producing stable ASS text, this
	// avoids tiny seams between adjacent canonical quarter curves.
	if (angle == 0.0)
		return {center_x + radius_x, center_y};
	if (angle == 90.0)
		return {center_x, center_y - radius_y};
	if (angle == 180.0)
		return {center_x - radius_x, center_y};
	if (angle == 270.0)
		return {center_x, center_y + radius_y};

	double radians = DegreesToRadians(angle);
	return {
		center_x + radius_x * std::cos(radians),
		center_y - radius_y * std::sin(radians),
	};
}

Point Interpolate(Point const& first, Point const& second, double t) {
	return {
		first.x + (second.x - first.x) * t,
		first.y + (second.y - first.y) * t,
	};
}

void SplitCubic(CubicSegment const& cubic, double t, CubicSegment& before, CubicSegment& after) {
	Point level1_0 = Interpolate(cubic.start, cubic.control1, t);
	Point level1_1 = Interpolate(cubic.control1, cubic.control2, t);
	Point level1_2 = Interpolate(cubic.control2, cubic.end, t);
	Point level2_0 = Interpolate(level1_0, level1_1, t);
	Point level2_1 = Interpolate(level1_1, level1_2, t);
	Point split = Interpolate(level2_0, level2_1, t);

	before = {cubic.start, level1_0, level2_0, split};
	after = {split, level2_1, level1_2, cubic.end};
}

CubicSegment CubicOnInterval(CubicSegment const& cubic, double from, double to) {
	from = std::clamp(from, 0.0, 1.0);
	to = std::clamp(to, from, 1.0);
	if (from == 0.0 && to == 1.0)
		return cubic;

	CubicSegment prefix;
	CubicSegment discarded;
	SplitCubic(cubic, to, prefix, discarded);
	if (from == 0.0)
		return prefix;

	CubicSegment interval;
	SplitCubic(prefix, from / to, discarded, interval);
	return interval;
}

CubicSegment ReverseCubic(CubicSegment const& cubic) {
	return {cubic.end, cubic.control2, cubic.control1, cubic.start};
}

CubicSegment CanonicalEllipseQuarter(Rect const& rect, int quadrant) {
	double radius_x = rect.width * 0.5;
	double radius_y = rect.height * 0.5;
	double center_x = rect.x + radius_x;
	double center_y = rect.y + radius_y;
	double tangent_x = radius_x * kBezierCircle;
	double tangent_y = radius_y * kBezierCircle;

	quadrant = ((quadrant % 4) + 4) % 4;
	switch (quadrant) {
		case 0:
			return {
				{center_x + radius_x, center_y},
				{center_x + radius_x, center_y - tangent_y},
				{center_x + tangent_x, center_y - radius_y},
				{center_x, center_y - radius_y},
			};
		case 1:
			return {
				{center_x, center_y - radius_y},
				{center_x - tangent_x, center_y - radius_y},
				{center_x - radius_x, center_y - tangent_y},
				{center_x - radius_x, center_y},
			};
		case 2:
			return {
				{center_x - radius_x, center_y},
				{center_x - radius_x, center_y + tangent_y},
				{center_x - tangent_x, center_y + radius_y},
				{center_x, center_y + radius_y},
			};
		default:
			return {
				{center_x, center_y + radius_y},
				{center_x + tangent_x, center_y + radius_y},
				{center_x + radius_x, center_y + tangent_y},
				{center_x + radius_x, center_y},
			};
	}
}

void EvaluateUnitQuarter(double t, Point& point, Point& derivative) {
	constexpr Point points[] = {
		{1.0, 0.0},
		{1.0, kBezierCircle},
		{kBezierCircle, 1.0},
		{0.0, 1.0},
	};
	double one_minus_t = 1.0 - t;
	double one_minus_t_squared = one_minus_t * one_minus_t;
	double t_squared = t * t;

	point = {
		points[0].x * one_minus_t_squared * one_minus_t
			+ 3.0 * points[1].x * one_minus_t_squared * t
			+ 3.0 * points[2].x * one_minus_t * t_squared
			+ points[3].x * t_squared * t,
		points[0].y * one_minus_t_squared * one_minus_t
			+ 3.0 * points[1].y * one_minus_t_squared * t
			+ 3.0 * points[2].y * one_minus_t * t_squared
			+ points[3].y * t_squared * t,
	};
	derivative = {
		3.0 * ((points[1].x - points[0].x) * one_minus_t_squared
			+ 2.0 * (points[2].x - points[1].x) * one_minus_t * t
			+ (points[3].x - points[2].x) * t_squared),
		3.0 * ((points[1].y - points[0].y) * one_minus_t_squared
			+ 2.0 * (points[2].y - points[1].y) * one_minus_t * t
			+ (points[3].y - points[2].y) * t_squared),
	};
}

double QuarterParameter(double angle_fraction) {
	angle_fraction = std::clamp(angle_fraction, 0.0, 1.0);
	if (angle_fraction == 0.0 || angle_fraction == 1.0)
		return angle_fraction;

	double radians = angle_fraction * kPi * 0.5;
	double target_cos = std::cos(radians);
	double target_sin = std::sin(radians);
	double low = 0.0;
	double high = 1.0;
	double t = angle_fraction;

	// Intersect the canonical cubic with the ray at the requested angle. The
	// signed cross product is monotone over a quarter, so bracketed Newton is
	// deterministic and keeps partial arcs on the same curve as a full ellipse.
	for (int iteration = 0; iteration < 8; ++iteration) {
		Point point;
		Point derivative;
		EvaluateUnitQuarter(t, point, derivative);
		double error = point.x * target_sin - point.y * target_cos;
		if (std::abs(error) <= 1e-15)
			return t;

		if (error > 0.0)
			low = t;
		else
			high = t;

		double slope = derivative.x * target_sin - derivative.y * target_cos;
		double candidate = t - error / slope;
		if (!std::isfinite(candidate) || candidate < low || candidate > high)
			candidate = (low + high) * 0.5;
		t = candidate;
	}
	return t;
}

void AppendMoveTo(PathData& path, PathState& state, Point const& point) {
	path.commands.push_back({PathVerb::MoveTo, point, {}, {}});
	state.has_current = true;
	state.current = point;
	state.has_subpath_start = true;
	state.subpath_start = point;
}

void AppendLineTo(PathData& path, PathState& state, Point const& point) {
	if (!state.has_current)
		AppendMoveTo(path, state, {});

	path.commands.push_back({PathVerb::LineTo, point, {}, {}});
	state.current = point;
}

void AppendCubicTo(PathData& path, PathState& state, Point const& c1, Point const& c2, Point const& end_point) {
	if (!state.has_current)
		AppendMoveTo(path, state, {});

	path.commands.push_back({PathVerb::CubicTo, c1, c2, end_point});
	state.current = end_point;
}

void AppendClose(PathData& path, PathState& state) {
	if (!state.has_subpath_start)
		return;

	path.commands.push_back({PathVerb::Close, {}, {}, {}});
	state.has_current = true;
	state.current = state.subpath_start;
}

void EnsureCurrent(PathState& state) {
	if (state.has_current)
		return;

	state.has_current = true;
	state.current = {};
	state.has_subpath_start = true;
	state.subpath_start = {};
}

PathState AnalyzePath(PathData const& path) {
	PathState state;
	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				state.has_current = true;
				state.current = command.p1;
				state.has_subpath_start = true;
				state.subpath_start = command.p1;
				break;
			case PathVerb::LineTo:
				EnsureCurrent(state);
				state.current = command.p1;
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
				EnsureCurrent(state);
				state.current = command.p2;
				break;
			case PathVerb::CubicTo:
				EnsureCurrent(state);
				state.current = command.p3;
				break;
			case PathVerb::Close:
				if (state.has_subpath_start) {
					state.has_current = true;
					state.current = state.subpath_start;
				}
				break;
		}
	}
	return state;
}

void AppendEllipseArcSpan(PathData& path, PathState& state, Rect const& rect,
	double start_angle, double end_angle, int quadrant) {
	double quarter_start = static_cast<double>(quadrant) * 90.0;
	double start_t = QuarterParameter((start_angle - quarter_start) / 90.0);
	double end_t = QuarterParameter((end_angle - quarter_start) / 90.0);
	bool reverse = start_t > end_t;
	CubicSegment span = reverse
		? ReverseCubic(CubicOnInterval(CanonicalEllipseQuarter(rect, quadrant), end_t, start_t))
		: CubicOnInterval(CanonicalEllipseQuarter(rect, quadrant), start_t, end_t);

	// The canonical cubic is only an approximation of an ellipse. Snap the
	// public arc endpoint to the exact requested angle while retaining the
	// canonical curve's controls, as adjacent quarter boundaries are exact.
	AppendCubicTo(path, state, span.control1, span.control2, EllipsePoint(rect, end_angle));
}

void AppendEllipseArc(PathData& path, PathState& state, Rect const& rect, double start_angle, double sweep_length, bool connect_from_current) {
	if (!std::isfinite(start_angle) || !std::isfinite(sweep_length))
		return;
	sweep_length = std::clamp(sweep_length, -360.0, 360.0);
	start_angle = NormalizeDegrees(start_angle);

	Point start = EllipsePoint(rect, start_angle);
	if (connect_from_current) {
		if (!state.has_current)
			AppendMoveTo(path, state, {});
		if (!SamePoint(state.current, start))
			AppendLineTo(path, state, start);
	}
	else {
		AppendMoveTo(path, state, start);
	}

	if (sweep_length == 0.0)
		return;

	int direction = sweep_length > 0.0 ? 1 : -1;
	double current_angle = start_angle;
	double remaining = std::abs(sweep_length);

	// An arc of at most one turn crosses no more than five quarter segments:
	// two partial end quarters and up to three complete quarters between them.
	for (int segment = 0; segment < 5 && remaining > 0.0; ++segment) {
		int quadrant;
		double boundary;
		if (direction > 0) {
			quadrant = static_cast<int>(std::floor(current_angle / 90.0));
			boundary = static_cast<double>(quadrant + 1) * 90.0;
		}
		else {
			quadrant = static_cast<int>(std::ceil(current_angle / 90.0)) - 1;
			boundary = static_cast<double>(quadrant) * 90.0;
		}

		double available = std::abs(boundary - current_angle);
		bool final_segment = remaining <= available;
		double step = final_segment ? remaining : available;
		double next_angle = final_segment
			? current_angle + static_cast<double>(direction) * step
			: boundary;
		AppendEllipseArcSpan(path, state, rect, current_angle, next_angle, quadrant);
		current_angle = next_angle;
		remaining = final_segment ? 0.0 : remaining - step;
	}
}
} // namespace

PathData MakeRect(double x, double y, double width, double height) {
	Rect rect = NormalizeRect(x, y, width, height);
	if (!RectIsFinite(rect))
		return {};
	PathData path;
	PathState state;
	AppendMoveTo(path, state, {rect.x, rect.y});
	AppendLineTo(path, state, {rect.x + rect.width, rect.y});
	AppendLineTo(path, state, {rect.x + rect.width, rect.y + rect.height});
	AppendLineTo(path, state, {rect.x, rect.y + rect.height});
	AppendClose(path, state);
	return path;
}

PathData MakeEllipse(double x, double y, double width, double height) {
	Rect rect = NormalizeRect(x, y, width, height);
	if (!RectIsFinite(rect))
		return {};
	PathData path;
	PathState state;
	// Match rect/rounded-rect winding and Qt's addEllipse orientation.
	AppendEllipseArc(path, state, rect, 0.0, -360.0, false);
	AppendClose(path, state);
	return path;
}

PathData MakeRoundedRect(double x, double y, double width, double height, double radius_x, double radius_y) {
	Rect rect = NormalizeRect(x, y, width, height);
	if (!RectIsFinite(rect) || !std::isfinite(radius_x) || !std::isfinite(radius_y))
		return {};
	radius_x = std::min(std::abs(radius_x), rect.width * 0.5);
	radius_y = std::min(std::abs(radius_y), rect.height * 0.5);
	if (radius_x <= 0.0 || radius_y <= 0.0)
		return MakeRect(rect.x, rect.y, rect.width, rect.height);

	double left = rect.x;
	double top = rect.y;
	double right = rect.x + rect.width;
	double bottom = rect.y + rect.height;
	double kx = radius_x * kBezierCircle;
	double ky = radius_y * kBezierCircle;

	PathData path;
	PathState state;
	AppendMoveTo(path, state, {left, top + radius_y});
	AppendCubicTo(path, state, {left, top + radius_y - ky}, {left + radius_x - kx, top}, {left + radius_x, top});
	AppendLineTo(path, state, {right - radius_x, top});
	AppendCubicTo(path, state, {right - radius_x + kx, top}, {right, top + radius_y - ky}, {right, top + radius_y});
	AppendLineTo(path, state, {right, bottom - radius_y});
	AppendCubicTo(path, state, {right, bottom - radius_y + ky}, {right - radius_x + kx, bottom}, {right - radius_x, bottom});
	AppendLineTo(path, state, {left + radius_x, bottom});
	AppendCubicTo(path, state, {left + radius_x - kx, bottom}, {left, bottom - radius_y + ky}, {left, bottom - radius_y});
	AppendLineTo(path, state, {left, top + radius_y});
	AppendClose(path, state);
	return path;
}

void AppendArcMoveTo(PathData& path, double x, double y, double width, double height, double angle) {
	if (!std::isfinite(angle))
		return;
	Rect rect {x, y, width, height};
	if (!RectIsFinite(rect))
		return;
	PathState state = AnalyzePath(path);
	AppendMoveTo(path, state, EllipsePoint(rect, angle));
}

void AppendArcTo(PathData& path, double x, double y, double width, double height, double start_angle, double sweep_length) {
	Rect rect {x, y, width, height};
	if (!RectIsFinite(rect))
		return;
	PathState state = AnalyzePath(path);
	AppendEllipseArc(path, state, rect, start_angle, sweep_length, true);
}

} // namespace drawing
} // namespace ass
} // namespace agi
