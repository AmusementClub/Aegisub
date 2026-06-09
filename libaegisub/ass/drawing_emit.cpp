#include "libaegisub/ass/drawing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agi {
namespace ass {
namespace drawing {

namespace {

constexpr double kAssGridScale = 64.0;
constexpr std::int64_t kAssGridScaleInt = 64;
constexpr std::int64_t kAssDecimalScale = 1000;
constexpr double kConicApproximationTolerance = 1.0 / 128.0;
constexpr int kConicMaxRecursionDepth = 10;

struct LoweringState {
	PathData lowered;
	bool preserve_empty_open_contours = false;
	bool has_current = false;
	Point current {};
	bool has_subpath_start = false;
	Point subpath_start {};
	bool figure_has_segments = false;
	std::size_t current_move_index = 0;
	bool last_emitted_was_line = false;
	Point last_line_start {};
};

struct CubicBezier {
	Point p0 {};
	Point p1 {};
	Point p2 {};
	Point p3 {};
};

struct ContourSlice {
	std::size_t begin = 0;
	std::size_t end = 0;
};

struct CompactSegment {
	PathVerb verb = PathVerb::LineTo;
	Point start {};
	Point c1 {};
	Point c2 {};
	Point end {};
};

struct FormattedPoint {
	std::string x;
	std::string y;
};

struct FormattedCompactSegment {
	PathVerb verb = PathVerb::LineTo;
	FormattedPoint start;
	FormattedPoint c1;
	FormattedPoint c2;
	FormattedPoint end;
};

struct RotatedSegmentTokenCursor {
	std::vector<FormattedCompactSegment> const& segments;
	std::size_t first = 0;
	std::size_t segment_offset = 0;
	int phase = 0;
	char last_command = 0;

	bool Next(std::string_view& token);
};

std::int64_t QuantizeCoordinate(double value) {
	if (!std::isfinite(value))
		return 0;

	return static_cast<std::int64_t>(std::llround(value * kAssGridScale));
}

Point PointFromGrid(std::int64_t x, std::int64_t y) {
	return {
		static_cast<double>(x) / kAssGridScale,
		static_cast<double>(y) / kAssGridScale,
	};
}

Point QuantizePoint(Point const& point) {
	return PointFromGrid(QuantizeCoordinate(point.x), QuantizeCoordinate(point.y));
}

bool PointIsFinite(Point const& point) {
	return std::isfinite(point.x) && std::isfinite(point.y);
}

bool SameQuantizedPoint(Point const& lhs, Point const& rhs) {
	return QuantizeCoordinate(lhs.x) == QuantizeCoordinate(rhs.x) &&
		QuantizeCoordinate(lhs.y) == QuantizeCoordinate(rhs.y);
}

bool CubicCollapsesToPoint(Point const& start, Point const& c1, Point const& c2, Point const& end) {
	return SameQuantizedPoint(start, c1) &&
		SameQuantizedPoint(start, c2) &&
		SameQuantizedPoint(start, end);
}

long double CrossGrid(std::int64_t ax, std::int64_t ay, std::int64_t bx, std::int64_t by) {
	return static_cast<long double>(ax) * static_cast<long double>(by) -
		static_cast<long double>(ay) * static_cast<long double>(bx);
}

long double DotGrid(std::int64_t ax, std::int64_t ay, std::int64_t bx, std::int64_t by) {
	return static_cast<long double>(ax) * static_cast<long double>(bx) +
		static_cast<long double>(ay) * static_cast<long double>(by);
}

bool LinePointsAreMergeable(Point const& start, Point const& middle, Point const& end) {
	auto x0 = QuantizeCoordinate(start.x);
	auto y0 = QuantizeCoordinate(start.y);
	auto x1 = QuantizeCoordinate(middle.x);
	auto y1 = QuantizeCoordinate(middle.y);
	auto x2 = QuantizeCoordinate(end.x);
	auto y2 = QuantizeCoordinate(end.y);

	auto ax = x1 - x0;
	auto ay = y1 - y0;
	auto bx = x2 - x1;
	auto by = y2 - y1;
	return CrossGrid(ax, ay, bx, by) == 0.0L && DotGrid(ax, ay, bx, by) >= 0.0L;
}

bool CubicCollapsesToLine(Point const& start, Point const& c1, Point const& c2, Point const& end) {
	auto x0 = QuantizeCoordinate(start.x);
	auto y0 = QuantizeCoordinate(start.y);
	auto x1 = QuantizeCoordinate(c1.x);
	auto y1 = QuantizeCoordinate(c1.y);
	auto x2 = QuantizeCoordinate(c2.x);
	auto y2 = QuantizeCoordinate(c2.y);
	auto x3 = QuantizeCoordinate(end.x);
	auto y3 = QuantizeCoordinate(end.y);

	auto dx = x3 - x0;
	auto dy = y3 - y0;
	auto denominator = DotGrid(dx, dy, dx, dy);
	if (denominator <= 0.0L)
		return false;

	if (CrossGrid(x1 - x0, y1 - y0, dx, dy) != 0.0L ||
		CrossGrid(x2 - x0, y2 - y0, dx, dy) != 0.0L)
		return false;

	auto dot1 = DotGrid(x1 - x0, y1 - y0, dx, dy);
	auto dot2 = DotGrid(x2 - x0, y2 - y0, dx, dy);
	return dot1 >= 0.0L && dot1 <= denominator &&
		dot2 >= 0.0L && dot2 <= denominator;
}

Point Add(Point const& lhs, Point const& rhs) {
	return {lhs.x + rhs.x, lhs.y + rhs.y};
}

Point Subtract(Point const& lhs, Point const& rhs) {
	return {lhs.x - rhs.x, lhs.y - rhs.y};
}

Point Scale(Point const& point, double factor) {
	return {point.x * factor, point.y * factor};
}

double DistanceSquared(Point const& lhs, Point const& rhs) {
	double dx = lhs.x - rhs.x;
	double dy = lhs.y - rhs.y;
	return dx * dx + dy * dy;
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

Point ConicPointAt(Point const& start, Point const& control, Point const& end, double weight, double t) {
	double mt = 1.0 - t;
	double a = mt * mt;
	double b = 2.0 * weight * t * mt;
	double c = t * t;
	double denominator = a + b + c;
	if (!(denominator > 0.0) || !std::isfinite(denominator))
		return end;

	return {
		(a * start.x + b * control.x + c * end.x) / denominator,
		(a * start.y + b * control.y + c * end.y) / denominator,
	};
}

Point ConicDerivativeAt(Point const& start, Point const& control, Point const& end, double weight, double t) {
	double mt = 1.0 - t;
	double a = mt * mt;
	double b = 2.0 * weight * t * mt;
	double c = t * t;
	double da = -2.0 * mt;
	double db = 2.0 * weight * (1.0 - 2.0 * t);
	double dc = 2.0 * t;
	double denominator = a + b + c;
	double denominator_derivative = da + db + dc;
	if (!(denominator > 0.0) || !std::isfinite(denominator))
		return {};

	Point numerator {
		a * start.x + b * control.x + c * end.x,
		a * start.y + b * control.y + c * end.y,
	};
	Point numerator_derivative {
		da * start.x + db * control.x + dc * end.x,
		da * start.y + db * control.y + dc * end.y,
	};
	double inverse_denominator_squared = 1.0 / (denominator * denominator);
	return {
		(numerator_derivative.x * denominator - numerator.x * denominator_derivative) * inverse_denominator_squared,
		(numerator_derivative.y * denominator - numerator.y * denominator_derivative) * inverse_denominator_squared,
	};
}

CubicBezier ApproximateConicSpanAsCubic(Point const& start,
	Point const& control,
	Point const& end,
	double weight,
	double t0,
	double t1) {
	Point span_start = ConicPointAt(start, control, end, weight, t0);
	Point span_end = ConicPointAt(start, control, end, weight, t1);
	Point derivative_start = ConicDerivativeAt(start, control, end, weight, t0);
	Point derivative_end = ConicDerivativeAt(start, control, end, weight, t1);
	double parameter_span = t1 - t0;

	return {
		span_start,
		Add(span_start, Scale(derivative_start, parameter_span / 3.0)),
		Subtract(span_end, Scale(derivative_end, parameter_span / 3.0)),
		span_end,
	};
}

double ConicSpanCubicErrorSquared(Point const& start,
	Point const& control,
	Point const& end,
	double weight,
	double t0,
	double t1,
	CubicBezier const& cubic) {
	double max_distance_squared = 0.0;
	constexpr double samples[] = {0.25, 0.5, 0.75};
	for (double sample : samples) {
		double t = t0 + (t1 - t0) * sample;
		max_distance_squared = std::max(max_distance_squared,
			DistanceSquared(ConicPointAt(start, control, end, weight, t), CubicPointAt(cubic, sample)));
	}
	return max_distance_squared;
}

void RemoveTrailingEmptyMove(LoweringState& state) {
	if (state.preserve_empty_open_contours)
		return;

	if (!state.lowered.commands.empty() &&
		state.lowered.commands.back().verb == PathVerb::MoveTo &&
		!state.figure_has_segments) {
		state.lowered.commands.pop_back();
	}
}

void EnsureCurrent(LoweringState& state) {
	if (state.has_current)
		return;

	state.current_move_index = state.lowered.commands.size();
	state.lowered.commands.push_back({PathVerb::MoveTo, {}, {}, {}});
	state.has_current = true;
	state.current = {};
	state.has_subpath_start = true;
	state.subpath_start = {};
	state.figure_has_segments = false;
	state.last_emitted_was_line = false;
}

void AppendMoveTo(LoweringState& state, Point point) {
	RemoveTrailingEmptyMove(state);
	point = QuantizePoint(point);
	state.current_move_index = state.lowered.commands.size();
	state.lowered.commands.push_back({PathVerb::MoveTo, point, {}, {}});
	state.has_current = true;
	state.current = point;
	state.has_subpath_start = true;
	state.subpath_start = point;
	state.figure_has_segments = false;
	state.last_emitted_was_line = false;
}

void AppendLineTo(LoweringState& state, Point point) {
	EnsureCurrent(state);
	point = QuantizePoint(point);
	if (SamePoint(state.current, point))
		return;

	if (state.last_emitted_was_line &&
		!state.lowered.commands.empty() &&
		state.lowered.commands.back().verb == PathVerb::LineTo &&
		LinePointsAreMergeable(state.last_line_start, state.current, point)) {
		state.lowered.commands.back().p1 = point;
		state.current = point;
		state.figure_has_segments = true;
		return;
	}

	state.last_line_start = state.current;
	state.lowered.commands.push_back({PathVerb::LineTo, point, {}, {}});
	state.current = point;
	state.figure_has_segments = true;
	state.last_emitted_was_line = true;
}

void AppendCubicTo(LoweringState& state, Point control1, Point control2, Point end_point) {
	EnsureCurrent(state);
	if (!PointIsFinite(control1) || !PointIsFinite(control2) || !PointIsFinite(end_point))
		return;

	control1 = QuantizePoint(control1);
	control2 = QuantizePoint(control2);
	end_point = QuantizePoint(end_point);

	if (CubicCollapsesToPoint(state.current, control1, control2, end_point))
		return;

	if (CubicCollapsesToLine(state.current, control1, control2, end_point)) {
		AppendLineTo(state, end_point);
		return;
	}

	state.lowered.commands.push_back({PathVerb::CubicTo, control1, control2, end_point});
	state.current = end_point;
	state.figure_has_segments = true;
	state.last_emitted_was_line = false;
}

void AppendConicSpan(LoweringState& state,
	Point const& start,
	Point const& control,
	Point const& end,
	double weight,
	double t0,
	double t1,
	int remaining_depth) {
	CubicBezier cubic = ApproximateConicSpanAsCubic(start, control, end, weight, t0, t1);
	double tolerance_squared = kConicApproximationTolerance * kConicApproximationTolerance;
	if (remaining_depth > 0 &&
		ConicSpanCubicErrorSquared(start, control, end, weight, t0, t1, cubic) > tolerance_squared) {
		double mid = (t0 + t1) * 0.5;
		AppendConicSpan(state, start, control, end, weight, t0, mid, remaining_depth - 1);
		AppendConicSpan(state, start, control, end, weight, mid, t1, remaining_depth - 1);
		return;
	}

	AppendCubicTo(state, cubic.p1, cubic.p2, cubic.p3);
}

void AppendQuadTo(LoweringState& state, Point control, Point end_point) {
	EnsureCurrent(state);
	if (!PointIsFinite(control) || !PointIsFinite(end_point))
		return;

	Point start = state.current;
	AppendCubicTo(state,
		Add(start, Scale(Subtract(control, start), 2.0 / 3.0)),
		Add(end_point, Scale(Subtract(control, end_point), 2.0 / 3.0)),
		end_point);
}

void AppendConicTo(LoweringState& state, Point control, Point end_point, double weight) {
	EnsureCurrent(state);
	if (!PointIsFinite(control) || !PointIsFinite(end_point) || !(weight > 0.0) || !std::isfinite(weight)) {
		AppendLineTo(state, end_point);
		return;
	}

	Point start = state.current;
	AppendConicSpan(state, start, control, end_point, weight, 0.0, 1.0, kConicMaxRecursionDepth);
}

void AppendClose(LoweringState& state, bool implicit_close_contours) {
	if (!state.has_subpath_start)
		return;

	if (!state.figure_has_segments) {
		RemoveTrailingEmptyMove(state);
		state.has_subpath_start = false;
		state.last_emitted_was_line = false;
		return;
	}

	if (implicit_close_contours && !SamePoint(state.current, state.subpath_start)) {
		auto reset_point = state.subpath_start;
		AppendMoveTo(state, reset_point);
		return;
	}

	if (!implicit_close_contours && !SamePoint(state.current, state.subpath_start))
		AppendLineTo(state, state.subpath_start);

	state.current = state.subpath_start;
	state.has_current = true;
	state.has_subpath_start = false;
	state.figure_has_segments = false;
	state.last_emitted_was_line = false;
}

std::string FormatCoordinate(double value) {
	auto grid = QuantizeCoordinate(value);
	if (grid == 0)
		return "0";

	bool negative = grid < 0;
	std::uint64_t magnitude = negative
		? static_cast<std::uint64_t>(-(grid + 1)) + 1
		: static_cast<std::uint64_t>(grid);

	auto whole = magnitude / kAssGridScaleInt;
	auto remainder = magnitude % kAssGridScaleInt;
	auto decimals = (remainder * kAssDecimalScale + kAssGridScaleInt / 2) / kAssGridScaleInt;
	if (decimals == kAssDecimalScale) {
		++whole;
		decimals = 0;
	}

	std::string text;
	if (negative)
		text.push_back('-');
	text += std::to_string(whole);
	if (decimals == 0)
		return text == "-0" ? "0" : text;

	std::string fraction = std::to_string(decimals);
	if (fraction.size() < 3)
		fraction.insert(fraction.begin(), 3 - fraction.size(), '0');
	while (!fraction.empty() && fraction.back() == '0')
		fraction.pop_back();

	text.push_back('.');
	text += fraction;
	return text == "-0" ? "0" : text;
}

FormattedPoint FormatPoint(Point const& point) {
	return {FormatCoordinate(point.x), FormatCoordinate(point.y)};
}

void AppendToken(std::string& text, std::string_view token) {
	if (!text.empty())
		text.push_back(' ');
	text.append(token);
}

void AppendPointText(std::string& text, Point const& point) {
	auto formatted = FormatPoint(point);
	AppendToken(text, formatted.x);
	AppendToken(text, formatted.y);
}

int CompareToken(std::string_view lhs, std::string_view rhs) {
	int value = lhs.compare(rhs);
	if (value < 0)
		return -1;
	if (value > 0)
		return 1;
	return 0;
}

int CompareFormattedPoint(FormattedPoint const& lhs, FormattedPoint const& rhs) {
	if (int value = CompareToken(lhs.x, rhs.x))
		return value;
	return CompareToken(lhs.y, rhs.y);
}

void AddTokenSize(std::size_t& chars, std::size_t& tokens, std::string_view token) {
	chars += token.size();
	++tokens;
}

void AddPointTokenSize(std::size_t& chars, std::size_t& tokens, FormattedPoint const& point) {
	AddTokenSize(chars, tokens, point.x);
	AddTokenSize(chars, tokens, point.y);
}

std::size_t JoinedTokenSize(std::size_t chars, std::size_t tokens) {
	return tokens == 0 ? 0 : chars + tokens - 1;
}

bool RotatedSegmentTokenCursor::Next(std::string_view& token) {
	if (phase == 0) {
		token = "m";
		phase = 1;
		return true;
	}

	if (phase == 1) {
		token = segments[first].start.x;
		phase = 2;
		return true;
	}

	if (phase == 2) {
		token = segments[first].start.y;
		phase = 3;
		return true;
	}

	while (segment_offset + 1 < segments.size()) {
		auto const& segment = segments[(first + segment_offset) % segments.size()];
		char command = segment.verb == PathVerb::LineTo ? 'l' : 'b';
		if (phase == 3) {
			phase = 4;
			if (last_command != command) {
				last_command = command;
				token = command == 'l' ? "l" : "b";
				return true;
			}
		}

		if (segment.verb == PathVerb::LineTo) {
			if (phase == 4) {
				token = segment.end.x;
				phase = 5;
				return true;
			}
			if (phase == 5) {
				token = segment.end.y;
				phase = 3;
				++segment_offset;
				return true;
			}
		} else {
			if (phase == 4) {
				token = segment.c1.x;
				phase = 5;
				return true;
			}
			if (phase == 5) {
				token = segment.c1.y;
				phase = 6;
				return true;
			}
			if (phase == 6) {
				token = segment.c2.x;
				phase = 7;
				return true;
			}
			if (phase == 7) {
				token = segment.c2.y;
				phase = 8;
				return true;
			}
			if (phase == 8) {
				token = segment.end.x;
				phase = 9;
				return true;
			}
			if (phase == 9) {
				token = segment.end.y;
				phase = 3;
				++segment_offset;
				return true;
			}
		}
	}

	return false;
}

std::string SerializeLoweredAss(PathData const& lowered) {
	std::string text;
	text.reserve(lowered.commands.size() * 24);

	char last_command = 0;
	for (auto const& command : lowered.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				if (last_command != 'm') {
					AppendToken(text, "m");
					last_command = 'm';
				}
				AppendPointText(text, command.p1);
				break;
			case PathVerb::LineTo:
				if (last_command != 'l') {
					AppendToken(text, "l");
					last_command = 'l';
				}
				AppendPointText(text, command.p1);
				break;
			case PathVerb::CubicTo:
				if (last_command != 'b') {
					AppendToken(text, "b");
					last_command = 'b';
				}
				AppendPointText(text, command.p1);
				AppendPointText(text, command.p2);
				AppendPointText(text, command.p3);
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::Close:
				break;
		}
	}

	return text;
}

bool SameSerializedPoint(Point const& lhs, Point const& rhs) {
	return QuantizeCoordinate(lhs.x) == QuantizeCoordinate(rhs.x) &&
		QuantizeCoordinate(lhs.y) == QuantizeCoordinate(rhs.y);
}

std::vector<ContourSlice> SplitContours(PathData const& path) {
	std::vector<ContourSlice> contours;
	std::size_t begin = path.commands.size();
	for (std::size_t index = 0; index < path.commands.size(); ++index) {
		if (path.commands[index].verb != PathVerb::MoveTo)
			continue;

		if (begin != path.commands.size())
			contours.push_back({begin, index});
		begin = index;
	}

	if (begin != path.commands.size())
		contours.push_back({begin, path.commands.size()});
	return contours;
}

bool IsLineOnlyContour(PathData const& path, ContourSlice contour) {
	if (contour.begin >= contour.end || path.commands[contour.begin].verb != PathVerb::MoveTo)
		return false;

	for (std::size_t index = contour.begin + 1; index < contour.end; ++index) {
		if (path.commands[index].verb != PathVerb::LineTo)
			return false;
	}
	return contour.end - contour.begin >= 3;
}

std::vector<Point> LineContourPoints(PathData const& path, ContourSlice contour) {
	std::vector<Point> points;
	points.reserve(contour.end - contour.begin);
	points.push_back(path.commands[contour.begin].p1);
	for (std::size_t index = contour.begin + 1; index < contour.end; ++index)
		points.push_back(path.commands[index].p1);

	if (points.size() > 1 && SameSerializedPoint(points.front(), points.back()))
		points.pop_back();
	return points;
}

void AppendLineContourAt(PathData& result, std::vector<Point> const& points, std::size_t start) {
	if (points.empty())
		return;

	result.commands.push_back({PathVerb::MoveTo, points[start], {}, {}});
	for (std::size_t offset = 1; offset < points.size(); ++offset) {
		std::size_t index = (start + offset) % points.size();
		result.commands.push_back({PathVerb::LineTo, points[index], {}, {}});
	}
}

std::vector<FormattedPoint> FormatContourPoints(std::vector<Point> const& points) {
	std::vector<FormattedPoint> formatted;
	formatted.reserve(points.size());
	for (auto const& point : points)
		formatted.push_back(FormatPoint(point));
	return formatted;
}

bool RotatedLineContourTextLess(std::vector<FormattedPoint> const& points, std::size_t lhs, std::size_t rhs) {
	if (int value = CompareFormattedPoint(points[lhs], points[rhs]))
		return value < 0;

	for (std::size_t offset = 1; offset < points.size(); ++offset) {
		std::size_t lhs_index = (lhs + offset) % points.size();
		std::size_t rhs_index = (rhs + offset) % points.size();
		if (int value = CompareFormattedPoint(points[lhs_index], points[rhs_index]))
			return value < 0;
	}

	return false;
}

std::size_t BestLineContourStart(std::vector<Point> const& points) {
	std::size_t best = 0;
	auto formatted = FormatContourPoints(points);
	for (std::size_t index = 1; index < points.size(); ++index) {
		if (RotatedLineContourTextLess(formatted, index, best))
			best = index;
	}
	return best;
}

void AppendLineContour(PathData& result, std::vector<Point> const& points) {
	if (points.empty())
		return;

	AppendLineContourAt(result, points, points.size() >= 3 ? BestLineContourStart(points) : 0);
}

bool BuildCompactSegments(PathData const& path, ContourSlice contour, std::vector<CompactSegment>& segments) {
	if (contour.begin >= contour.end || path.commands[contour.begin].verb != PathVerb::MoveTo)
		return false;

	Point start = path.commands[contour.begin].p1;
	Point current = start;
	for (std::size_t index = contour.begin + 1; index < contour.end; ++index) {
		auto const& command = path.commands[index];
		switch (command.verb) {
			case PathVerb::LineTo:
				if (!SameSerializedPoint(current, command.p1))
					segments.push_back({PathVerb::LineTo, current, {}, {}, command.p1});
				current = command.p1;
				break;
			case PathVerb::CubicTo:
				segments.push_back({PathVerb::CubicTo, current, command.p1, command.p2, command.p3});
				current = command.p3;
				break;
			case PathVerb::MoveTo:
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
			case PathVerb::Close:
				return false;
		}
	}

	if (!SameSerializedPoint(current, start))
		segments.push_back({PathVerb::LineTo, current, {}, {}, start});

	return segments.size() >= 2;
}

void AppendRotatedSegments(PathData& result, std::vector<CompactSegment> const& segments, std::size_t omitted_line) {
	std::size_t first = (omitted_line + 1) % segments.size();
	result.commands.push_back({PathVerb::MoveTo, segments[first].start, {}, {}});
	for (std::size_t offset = 0; offset + 1 < segments.size(); ++offset) {
		auto const& segment = segments[(first + offset) % segments.size()];
		if (segment.verb == PathVerb::LineTo)
			result.commands.push_back({PathVerb::LineTo, segment.end, {}, {}});
		else if (segment.verb == PathVerb::CubicTo)
			result.commands.push_back({PathVerb::CubicTo, segment.c1, segment.c2, segment.end});
	}
}

std::vector<FormattedCompactSegment> FormatCompactSegments(std::vector<CompactSegment> const& segments) {
	std::vector<FormattedCompactSegment> formatted;
	formatted.reserve(segments.size());
	for (auto const& segment : segments) {
		FormattedCompactSegment item;
		item.verb = segment.verb;
		item.start = FormatPoint(segment.start);
		item.end = FormatPoint(segment.end);
		if (segment.verb == PathVerb::CubicTo) {
			item.c1 = FormatPoint(segment.c1);
			item.c2 = FormatPoint(segment.c2);
		}
		formatted.push_back(std::move(item));
	}
	return formatted;
}

std::size_t RotatedSegmentTextSize(std::vector<FormattedCompactSegment> const& segments, std::size_t omitted_line) {
	std::size_t chars = 0;
	std::size_t tokens = 0;
	std::size_t first = (omitted_line + 1) % segments.size();

	AddTokenSize(chars, tokens, "m");
	AddPointTokenSize(chars, tokens, segments[first].start);

	char last_command = 0;
	for (std::size_t offset = 0; offset + 1 < segments.size(); ++offset) {
		auto const& segment = segments[(first + offset) % segments.size()];
		char command = segment.verb == PathVerb::LineTo ? 'l' : 'b';
		if (last_command != command) {
			AddTokenSize(chars, tokens, command == 'l' ? "l" : "b");
			last_command = command;
		}

		if (segment.verb == PathVerb::LineTo) {
			AddPointTokenSize(chars, tokens, segment.end);
		} else {
			AddPointTokenSize(chars, tokens, segment.c1);
			AddPointTokenSize(chars, tokens, segment.c2);
			AddPointTokenSize(chars, tokens, segment.end);
		}
	}

	return JoinedTokenSize(chars, tokens);
}

bool RotatedSegmentTextLess(std::vector<FormattedCompactSegment> const& segments, std::size_t lhs, std::size_t rhs) {
	RotatedSegmentTokenCursor lhs_cursor {segments, (lhs + 1) % segments.size()};
	RotatedSegmentTokenCursor rhs_cursor {segments, (rhs + 1) % segments.size()};

	std::string_view lhs_token;
	std::string_view rhs_token;
	for (;;) {
		bool has_lhs = lhs_cursor.Next(lhs_token);
		bool has_rhs = rhs_cursor.Next(rhs_token);
		if (!has_lhs || !has_rhs)
			return has_rhs;
		if (int value = CompareToken(lhs_token, rhs_token))
			return value < 0;
	}
}

bool TryAppendCompactMixedContour(PathData& result, PathData const& path, ContourSlice contour) {
	std::vector<CompactSegment> segments;
	if (!BuildCompactSegments(path, contour, segments))
		return false;

	bool has_cubic = std::any_of(segments.begin(), segments.end(), [](CompactSegment const& segment) {
		return segment.verb == PathVerb::CubicTo;
	});
	if (!has_cubic)
		return false;

	auto formatted = FormatCompactSegments(segments);
	std::size_t best_omitted = segments.size();
	std::size_t best_cost = std::numeric_limits<std::size_t>::max();
	for (std::size_t index = 0; index < segments.size(); ++index) {
		if (segments[index].verb != PathVerb::LineTo)
			continue;

		std::size_t cost = RotatedSegmentTextSize(formatted, index);
		if (cost < best_cost || (cost == best_cost && (best_omitted == segments.size() || RotatedSegmentTextLess(formatted, index, best_omitted)))) {
			best_omitted = index;
			best_cost = cost;
		}
	}

	if (best_omitted == segments.size())
		return false;

	AppendRotatedSegments(result, segments, best_omitted);
	return true;
}

void AppendNonLineContour(PathData& result, PathData const& path, ContourSlice contour) {
	if (contour.begin >= contour.end)
		return;

	std::size_t end = contour.end;
	Point start = path.commands[contour.begin].p1;
	if (end > contour.begin + 1 &&
		path.commands[end - 1].verb == PathVerb::LineTo &&
		SameSerializedPoint(path.commands[end - 1].p1, start)) {
		--end;
	}

	for (std::size_t index = contour.begin; index < end; ++index)
		result.commands.push_back(path.commands[index]);
}

PathData CompactFilledPath(PathData const& path) {
	PathData lowered = LowerForAss(path, true);
	PathData compacted;
	compacted.winding_fill = lowered.winding_fill;
	compacted.commands.reserve(lowered.commands.size());

	for (auto const& contour : SplitContours(lowered)) {
		if (IsLineOnlyContour(lowered, contour))
			AppendLineContour(compacted, LineContourPoints(lowered, contour));
		else if (TryAppendCompactMixedContour(compacted, lowered, contour))
			continue;
		else
			AppendNonLineContour(compacted, lowered, contour);
	}

	return compacted;
}

} // namespace

PathData LowerForAss(PathData const& path, bool implicit_close_contours) {
	LoweringState state;
	state.lowered.winding_fill = path.winding_fill;
	state.lowered.commands.reserve(path.commands.size());

	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				if (PointIsFinite(command.p1))
					AppendMoveTo(state, command.p1);
				break;
			case PathVerb::LineTo:
				if (PointIsFinite(command.p1))
					AppendLineTo(state, command.p1);
				break;
			case PathVerb::QuadTo:
				AppendQuadTo(state, command.p1, command.p2);
				break;
			case PathVerb::ConicTo:
				AppendConicTo(state, command.p1, command.p2, command.weight);
				break;
			case PathVerb::CubicTo:
				AppendCubicTo(state, command.p1, command.p2, command.p3);
				break;
			case PathVerb::Close:
				AppendClose(state, implicit_close_contours);
				break;
		}
	}

	RemoveTrailingEmptyMove(state);
	return state.lowered;
}

std::string SerializeAss(PathData const& path) {
	LoweringState state;
	state.preserve_empty_open_contours = true;
	state.lowered.winding_fill = path.winding_fill;
	state.lowered.commands.reserve(path.commands.size());

	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
				if (PointIsFinite(command.p1))
					AppendMoveTo(state, command.p1);
				break;
			case PathVerb::LineTo:
				if (PointIsFinite(command.p1))
					AppendLineTo(state, command.p1);
				break;
			case PathVerb::QuadTo:
				AppendQuadTo(state, command.p1, command.p2);
				break;
			case PathVerb::ConicTo:
				AppendConicTo(state, command.p1, command.p2, command.weight);
				break;
			case PathVerb::CubicTo:
				AppendCubicTo(state, command.p1, command.p2, command.p3);
				break;
			case PathVerb::Close:
				AppendClose(state, false);
				break;
		}
	}

	return SerializeLoweredAss(state.lowered);
}

std::string SerializeAssFilled(PathData const& path) {
	return SerializeLoweredAss(LowerForAss(path, true));
}

std::string SerializeAssCompactFilled(PathData const& path) {
	return SerializeLoweredAss(CompactFilledPath(path));
}

std::string CompactAss(std::string_view ass_shape, AssDrawingCompatMode compat_mode) {
	return SerializeAssCompactFilled(ParseAss(ass_shape, compat_mode));
}

} // namespace drawing
} // namespace ass
} // namespace agi
