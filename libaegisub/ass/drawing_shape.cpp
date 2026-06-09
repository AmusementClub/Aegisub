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

double DegreesToRadians(double degrees) {
	return degrees * kPi / 180.0;
}

Point EllipsePoint(Rect const& rect, double angle_degrees) {
	double radians = DegreesToRadians(angle_degrees);
	double radius_x = rect.width * 0.5;
	double radius_y = rect.height * 0.5;
	double center_x = rect.x + radius_x;
	double center_y = rect.y + radius_y;
	return {
		center_x + radius_x * std::cos(radians),
		center_y - radius_y * std::sin(radians),
	};
}

Point EllipseDerivative(Rect const& rect, double angle_degrees) {
	double radians = DegreesToRadians(angle_degrees);
	return {
		-rect.width * 0.5 * std::sin(radians),
		-rect.height * 0.5 * std::cos(radians),
	};
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

void AppendEllipseArcSpan(PathData& path, PathState& state, Rect const& rect, double start_angle, double sweep_angle) {
	Point start = EllipsePoint(rect, start_angle);
	Point end = EllipsePoint(rect, start_angle + sweep_angle);
	Point derivative_start = EllipseDerivative(rect, start_angle);
	Point derivative_end = EllipseDerivative(rect, start_angle + sweep_angle);
	double radians = DegreesToRadians(sweep_angle);
	double alpha = 4.0 / 3.0 * std::tan(radians * 0.25);

	AppendCubicTo(path,
		state,
		{start.x + derivative_start.x * alpha, start.y + derivative_start.y * alpha},
		{end.x - derivative_end.x * alpha, end.y - derivative_end.y * alpha},
		end);
}

void AppendEllipseArc(PathData& path, PathState& state, Rect const& rect, double start_angle, double sweep_length, bool connect_from_current) {
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

	if (std::abs(sweep_length) <= kPointEpsilon)
		return;

	int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweep_length) / 90.0)));
	double span = sweep_length / static_cast<double>(segments);
	for (int index = 0; index < segments; ++index)
		AppendEllipseArcSpan(path, state, rect, start_angle + span * static_cast<double>(index), span);
}

} // namespace

PathData MakeRect(double x, double y, double width, double height) {
	Rect rect = NormalizeRect(x, y, width, height);
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
	PathData path;
	PathState state;
	AppendEllipseArc(path, state, rect, 0.0, 360.0, false);
	AppendClose(path, state);
	return path;
}

PathData MakeRoundedRect(double x, double y, double width, double height, double radius_x, double radius_y) {
	Rect rect = NormalizeRect(x, y, width, height);
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
	PathState state = AnalyzePath(path);
	AppendMoveTo(path, state, EllipsePoint(NormalizeRect(x, y, width, height), angle));
}

void AppendArcTo(PathData& path, double x, double y, double width, double height, double start_angle, double sweep_length) {
	PathState state = AnalyzePath(path);
	AppendEllipseArc(path, state, NormalizeRect(x, y, width, height), start_angle, sweep_length, true);
}

} // namespace drawing
} // namespace ass
} // namespace agi
