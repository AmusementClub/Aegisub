#include "libaegisub/ass/drawing.h"

#include <array>
#include <cassert>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace agi {
namespace ass {
namespace drawing {

namespace {

constexpr double kD6Scale = 64.0;

struct PathState {
	bool has_current = false;
	Point current {};
	bool has_subpath_start = false;
	Point subpath_start {};
};

enum class DrawingTokenType {
	Move,
	MoveNC,
	Line,
	CubicBezier,
	BSpline,
	ExtendSpline,
};

struct DrawingToken {
	DrawingTokenType type {};
	Point point {};
};

struct DrawingCompatBehavior {
	bool bridge_pending_move_nc = false;
};

struct PathBuildBehavior {
	bool auto_close_contours = true;
	bool preserve_dangling_anchor = true;
};

struct HighlightState {
	bool has_root = false;
	bool saw_move = false;
	std::size_t point_count = 0;
	bool spline_active = false;
};

struct HighlightCoord {
	std::size_t whitespace_length = 0;
	std::size_t number_length = 0;
};

void AddLexeme(std::vector<Lexeme>& lexemes, std::size_t length, LexemeType type) {
	if (!length)
		return;

	if (!lexemes.empty() && lexemes.back().type == type)
		lexemes.back().length += length;
	else
		lexemes.push_back({type, length});
}

bool IsWhitespace(char c) {
	return std::isspace(static_cast<unsigned char>(c)) != 0;
}

void SkipWhitespace(char const*& cursor, char const* end) {
	while (cursor < end && IsWhitespace(*cursor))
		++cursor;
}

bool IsDrawingCommand(unsigned char raw) {
	switch (raw) {
		case 'm':
		case 'n':
		case 'l':
		case 'b':
		case 's':
		case 'p':
		case 'c':
			return true;
		default:
			return false;
	}
}

bool ReadNumber(char const* cursor, char const* end, char const*& number_end, double* value = nullptr) {
	if (cursor >= end || std::isalpha(static_cast<unsigned char>(*cursor)) != 0)
		return false;

	// Floating-point from_chars is bounded and locale independent. It follows
	// the strtod grammar except for a leading '+', which ASS accepts.
	char const* parse_start = cursor;
	if (*parse_start == '+') {
		++parse_start;
		if (parse_start == end)
			return false;
	}

	double parsed = 0.0;
	auto parsed_result = std::from_chars(parse_start, end, parsed, std::chars_format::general);
	if (parsed_result.ptr == parse_start || parsed_result.ec != std::errc() || !std::isfinite(parsed))
		return false;

	if (value)
		*value = parsed;
	number_end = parsed_result.ptr;
	return true;
}

bool TryReadDouble(char const*& cursor, char const* end, double& value) {
	SkipWhitespace(cursor, end);
	char const* number_end = nullptr;
	if (!ReadNumber(cursor, end, number_end, &value))
		return false;

	cursor = number_end;
	return true;
}

DrawingCompatBehavior GetDrawingCompatBehavior(AssDrawingCompatMode compat_mode) {
	DrawingCompatBehavior behavior;
	behavior.bridge_pending_move_nc = compat_mode == AssDrawingCompatMode::VsFilter;
	return behavior;
}

PathBuildBehavior GetPathBuildBehavior(AssDrawingPathMode path_mode) {
	PathBuildBehavior behavior;
	behavior.auto_close_contours = path_mode == AssDrawingPathMode::FilledContours;
	behavior.preserve_dangling_anchor = path_mode == AssDrawingPathMode::PreserveOpenContours;
	return behavior;
}

bool QuantizeToD6(double value, AssDrawingCompatMode compat_mode, double& quantized) {
	double input = value * kD6Scale;
	if (!std::isfinite(input))
		return false;

	double scaled = 0.0;
	switch (compat_mode) {
		case AssDrawingCompatMode::VsFilter:
			scaled = std::trunc(input);
			break;
		case AssDrawingCompatMode::Libass:
			scaled = std::nearbyint(input);
			break;
		default:
			scaled = std::trunc(input);
			break;
	}

	quantized = scaled / kD6Scale;
	return std::isfinite(quantized);
}

bool TryReadPoint(char const*& cursor, char const* end, Point& point, AssDrawingCompatMode compat_mode) {
	double x = 0.0;
	double y = 0.0;
	if (!TryReadDouble(cursor, end, x) || !TryReadDouble(cursor, end, y))
		return false;

	return QuantizeToD6(x, compat_mode, point.x) && QuantizeToD6(y, compat_mode, point.y);
}

void AppendToken(std::vector<DrawingToken>& tokens, DrawingTokenType type, Point const& point) {
	tokens.push_back({type, point});
}

std::size_t AddPointBatches(char const*& cursor,
	char const* end,
	std::vector<DrawingToken>& tokens,
	DrawingTokenType type,
	std::size_t batch_size,
	AssDrawingCompatMode compat_mode,
	std::size_t max_batches = std::numeric_limits<std::size_t>::max()) {
	assert(batch_size <= 3);
	std::array<Point, 3> batch {};

	std::size_t batch_count = 0;
	std::size_t batches = 0;
	std::size_t point_count = 0;
	Point point;
	while (batches < max_batches && TryReadPoint(cursor, end, point, compat_mode)) {
		batch[batch_count++] = point;

		if (batch_count != batch_size)
			continue;

		for (std::size_t i = 0; i < batch_size; ++i)
			AppendToken(tokens, type, batch[i]);
		point_count += batch_size;
		batch_count = 0;
		++batches;
	}

	return point_count;
}

std::vector<DrawingToken> TokenizeCompatibleDrawingTokens(std::string_view ass_shape, AssDrawingCompatMode compat_mode) {
	std::string text(ass_shape);
	char const* cursor = text.c_str();
	char const* end = cursor + text.size();
	std::vector<DrawingToken> tokens;
	tokens.reserve(64);

	bool has_root = false;
	bool saw_move_command = false;
	std::size_t point_count = 0;
	std::size_t spline_start_index = std::numeric_limits<std::size_t>::max();

	while (true) {
		SkipWhitespace(cursor, end);
		if (cursor >= end)
			break;

		auto raw = static_cast<unsigned char>(*cursor);
		if (!IsDrawingCommand(raw)) {
			++cursor;
			continue;
		}

		char command = static_cast<char>(raw);
		++cursor;

		switch (command) {
			case 'm':
				saw_move_command = true;
				if (!has_root) {
					Point point;
					if (!TryReadPoint(cursor, end, point, compat_mode))
						continue;
					AppendToken(tokens, DrawingTokenType::Move, point);
					has_root = true;
					point_count = 1;
				}
				point_count += AddPointBatches(cursor, end, tokens, DrawingTokenType::Move, 1, compat_mode);
				break;
			case 'n':
				if (!has_root) {
					Point point;
					if (!TryReadPoint(cursor, end, point, compat_mode))
						continue;
					if (!saw_move_command)
						return {};
					AppendToken(tokens, DrawingTokenType::MoveNC, point);
					has_root = true;
					point_count = 1;
				}
				point_count += AddPointBatches(cursor, end, tokens, DrawingTokenType::MoveNC, 1, compat_mode);
				break;
			case 'l':
				if (!has_root)
					continue;
				point_count += AddPointBatches(cursor, end, tokens, DrawingTokenType::Line, 1, compat_mode);
				break;
			case 'b':
				if (!has_root)
					continue;
				point_count += AddPointBatches(cursor, end, tokens, DrawingTokenType::CubicBezier, 3, compat_mode);
				break;
			case 's':
				if (!has_root)
					continue;
				spline_start_index = tokens.empty() ? std::numeric_limits<std::size_t>::max() : tokens.size() - 1;
				if (AddPointBatches(cursor, end, tokens, DrawingTokenType::BSpline, 3, compat_mode, 1) < 3) {
					spline_start_index = std::numeric_limits<std::size_t>::max();
					break;
				}
				point_count += 3;
				point_count += AddPointBatches(cursor, end, tokens, DrawingTokenType::ExtendSpline, 1, compat_mode);
				break;
			case 'p':
				if (point_count < 3)
					continue;
				point_count += AddPointBatches(cursor, end, tokens, DrawingTokenType::ExtendSpline, 1, compat_mode);
				break;
			case 'c':
				if (spline_start_index != std::numeric_limits<std::size_t>::max() &&
					spline_start_index + 2 < tokens.size()) {
					for (std::size_t offset = 0; offset < 3; ++offset)
						AppendToken(tokens, DrawingTokenType::ExtendSpline, tokens[spline_start_index + offset].point);
					spline_start_index = std::numeric_limits<std::size_t>::max();
				}
				break;
			default:
				break;
		}
	}

	return tokens;
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

void ConvertSplineCurve(Point& p0, Point& p1, Point& p2, Point& p3) {
	auto to_d6 = [](double value) -> std::int64_t {
		return static_cast<std::int64_t>(std::llround(value * kD6Scale));
	};
	auto from_d6 = [](std::int64_t x, std::int64_t y) -> Point {
		return {static_cast<double>(x) / kD6Scale, static_cast<double>(y) / kD6Scale};
	};

	std::int64_t x0 = to_d6(p0.x);
	std::int64_t y0 = to_d6(p0.y);
	std::int64_t x1 = to_d6(p1.x);
	std::int64_t y1 = to_d6(p1.y);
	std::int64_t x2 = to_d6(p2.x);
	std::int64_t y2 = to_d6(p2.y);
	std::int64_t x3 = to_d6(p3.x);
	std::int64_t y3 = to_d6(p3.y);

	std::int64_t x01 = (x1 - x0) / 3;
	std::int64_t y01 = (y1 - y0) / 3;
	std::int64_t x12 = (x2 - x1) / 3;
	std::int64_t y12 = (y2 - y1) / 3;
	std::int64_t x23 = (x3 - x2) / 3;
	std::int64_t y23 = (y3 - y2) / 3;

	p0 = from_d6(x1 + ((x12 - x01) >> 1), y1 + ((y12 - y01) >> 1));
	p3 = from_d6(x2 + ((x23 - x12) >> 1), y2 + ((y23 - y12) >> 1));
	p1 = from_d6(x1 + x12, y1 + y12);
	p2 = from_d6(x2 - x12, y2 - y12);
}

void EnsureSegmentStart(PathData& path,
	PathState& state,
	Point const& segment_start,
	bool bridge_pending,
	bool& started) {
	if (!started) {
		AppendMoveTo(path, state, segment_start);
		started = true;
		return;
	}

	if (bridge_pending && !SamePoint(state.current, segment_start))
		AppendLineTo(path, state, segment_start);
}

void AppendCurveFromTokens(PathData& path,
	PathState& state,
	Point const& p0,
	Point const& p1,
	Point const& p2,
	Point const& p3,
	bool bridge_pending,
	bool spline,
	bool& started) {
	Point c0 = p0;
	Point c1 = p1;
	Point c2 = p2;
	Point c3 = p3;

	if (spline)
		ConvertSplineCurve(c0, c1, c2, c3);

	EnsureSegmentStart(path, state, c0, bridge_pending, started);
	AppendCubicTo(path, state, c1, c2, c3);
}

void EndContour(PathData& path, PathState& state, bool auto_close_contours, bool& started) {
	if (!started)
		return;

	if (auto_close_contours)
		AppendClose(path, state);
	started = false;
}

PathData BuildPathFromTokens(std::vector<DrawingToken> const& tokens,
	DrawingCompatBehavior const& compat_behavior,
	PathBuildBehavior const& path_behavior) {
	PathData path;
	PathState state;
	bool started = false;
	Point pen {};
	bool has_pending_pen = false;
	bool pending_vsfilter_bridge = false;

	for (std::size_t index = 0; index < tokens.size();) {
		auto const& token = tokens[index];
		switch (token.type) {
			case DrawingTokenType::MoveNC:
				pen = token.point;
				has_pending_pen = true;
				pending_vsfilter_bridge = compat_behavior.bridge_pending_move_nc;
				++index;
				break;
			case DrawingTokenType::Move:
				pen = token.point;
				has_pending_pen = true;
				EndContour(path, state, path_behavior.auto_close_contours, started);
				pending_vsfilter_bridge = false;
				++index;
				break;
			case DrawingTokenType::Line:
				EnsureSegmentStart(path, state, pen, pending_vsfilter_bridge, started);
				AppendLineTo(path, state, token.point);
				pen = token.point;
				has_pending_pen = false;
				pending_vsfilter_bridge = false;
				++index;
				break;
			case DrawingTokenType::CubicBezier:
				if (index > 0 && index + 2 < tokens.size()) {
					AppendCurveFromTokens(path,
						state,
						tokens[index - 1].point,
						tokens[index].point,
						tokens[index + 1].point,
						tokens[index + 2].point,
						pending_vsfilter_bridge,
						false,
						started);
					pen = state.current;
					has_pending_pen = false;
					pending_vsfilter_bridge = false;
				}
				index += 3;
				break;
			case DrawingTokenType::BSpline:
				if (index > 0 && index + 2 < tokens.size()) {
					AppendCurveFromTokens(path,
						state,
						tokens[index - 1].point,
						tokens[index].point,
						tokens[index + 1].point,
						tokens[index + 2].point,
						pending_vsfilter_bridge,
						true,
						started);
					pen = state.current;
					has_pending_pen = false;
					pending_vsfilter_bridge = false;
				}
				index += 3;
				break;
			case DrawingTokenType::ExtendSpline:
				if (index >= 3) {
					AppendCurveFromTokens(path,
						state,
						tokens[index - 3].point,
						tokens[index - 2].point,
						tokens[index - 1].point,
						tokens[index].point,
						pending_vsfilter_bridge,
						true,
						started);
					pen = state.current;
					has_pending_pen = false;
					pending_vsfilter_bridge = false;
				}
				++index;
				break;
		}
	}

	EndContour(path, state, path_behavior.auto_close_contours, started);
	if (!started && has_pending_pen && path_behavior.preserve_dangling_anchor)
		AppendMoveTo(path, state, pen);

	return path;
}

bool IsCommandAllowed(char command, HighlightState const& state) {
	switch (command) {
		case 'm':
			return true;
		case 'n':
			return state.has_root || state.saw_move;
		case 'l':
		case 'b':
		case 's':
			return state.has_root;
		case 'p':
			return state.point_count >= 3;
		case 'c':
			return state.spline_active;
		default:
			return false;
	}
}

void MarkPairedCoordinates(std::vector<LexemeType>& styles,
	std::size_t first,
	std::size_t count,
	bool mark_cubic_endpoints) {
	for (std::size_t i = first; i < first + count; ++i) {
		bool is_x = (i - first) % 2 == 0;
		if (mark_cubic_endpoints && (i - first) % 6 >= 4)
			styles[i] = is_x ? LexemeType::EndpointX : LexemeType::EndpointY;
		else
			styles[i] = is_x ? LexemeType::X : LexemeType::Y;
	}
}

void StyleDrawingCoordinates(std::vector<Lexeme>& lexemes,
	char command,
	bool command_allowed,
	std::vector<HighlightCoord> const& coords,
	std::size_t trailing_whitespace,
	HighlightState& state) {
	std::vector<LexemeType> styles(coords.size(), LexemeType::Error);
	std::size_t complete_coords = 0;

	if (command_allowed) {
		switch (command) {
			case 'm':
			case 'n':
			case 'l':
			case 'p':
				complete_coords = coords.size() - coords.size() % 2;
				MarkPairedCoordinates(styles, 0, complete_coords, false);
				break;
			case 'b':
				complete_coords = coords.size() / 6 * 6;
				MarkPairedCoordinates(styles, 0, complete_coords, true);
				break;
			case 's':
				if (coords.size() >= 6) {
					complete_coords = 6 + (coords.size() - 6) / 2 * 2;
					MarkPairedCoordinates(styles, 0, complete_coords, false);
				}
				break;
			case 'c':
			default:
				break;
		}
	}

	LexemeType previous_coord_style = LexemeType::Normal;
	for (std::size_t i = 0; i < coords.size(); ++i) {
		auto whitespace_style = previous_coord_style == LexemeType::EndpointX && styles[i] == LexemeType::EndpointY
			? LexemeType::EndpointX
			: LexemeType::Normal;
		AddLexeme(lexemes, coords[i].whitespace_length, whitespace_style);
		AddLexeme(lexemes, coords[i].number_length, styles[i]);
		previous_coord_style = styles[i];
	}
	AddLexeme(lexemes, trailing_whitespace, LexemeType::Normal);

	if (!command_allowed || !complete_coords)
		return;

	switch (command) {
		case 'm':
			state.saw_move = true;
			state.has_root = true;
			state.point_count += complete_coords / 2;
			state.spline_active = false;
			break;
		case 'n':
			state.has_root = true;
			state.point_count += complete_coords / 2;
			state.spline_active = false;
			break;
		case 'l':
			state.point_count += complete_coords / 2;
			state.spline_active = false;
			break;
		case 'b':
			state.point_count += complete_coords / 2;
			state.spline_active = false;
			break;
		case 's':
			state.point_count += complete_coords / 2;
			state.spline_active = true;
			break;
		case 'p':
			state.point_count += complete_coords / 2;
			break;
		default:
			break;
	}
}

char const* LexDrawingCommand(std::vector<Lexeme>& lexemes, char const* cursor, char const* end, HighlightState& state) {
	char command = *cursor++;
	if (command == 'm')
		state.saw_move = true;

	bool command_allowed = IsCommandAllowed(command, state);
	AddLexeme(lexemes, 1, command_allowed ? LexemeType::Command : LexemeType::Error);

	if (command == 'c') {
		if (command_allowed)
			state.spline_active = false;
		return cursor;
	}

	std::vector<HighlightCoord> coords;
	std::size_t trailing_whitespace = 0;
	while (cursor < end) {
		char const* whitespace_start = cursor;
		while (cursor < end && IsWhitespace(*cursor))
			++cursor;
		std::size_t whitespace_length = cursor - whitespace_start;

		if (cursor >= end) {
			trailing_whitespace = whitespace_length;
			break;
		}

		if (IsDrawingCommand(static_cast<unsigned char>(*cursor)) ||
			std::isalpha(static_cast<unsigned char>(*cursor)) != 0) {
			trailing_whitespace = whitespace_length;
			break;
		}

		char const* number_end = nullptr;
		if (!ReadNumber(cursor, end, number_end)) {
			trailing_whitespace = whitespace_length;
			break;
		}

		coords.push_back({whitespace_length, static_cast<std::size_t>(number_end - cursor)});
		cursor = number_end;
	}

	StyleDrawingCoordinates(lexemes, command, command_allowed, coords, trailing_whitespace, state);
	return cursor;
}

} // namespace

bool AlmostEqual(double lhs, double rhs, double epsilon) {
	return std::abs(lhs - rhs) <= epsilon;
}

bool SamePoint(Point const& lhs, Point const& rhs, double epsilon) {
	return AlmostEqual(lhs.x, rhs.x, epsilon) && AlmostEqual(lhs.y, rhs.y, epsilon);
}

AssDrawingCompatMode SanitizeAssDrawingCompatMode(int raw_mode) {
	switch (static_cast<AssDrawingCompatMode>(raw_mode)) {
		case AssDrawingCompatMode::VsFilter:
			return AssDrawingCompatMode::VsFilter;
		case AssDrawingCompatMode::Libass:
			return AssDrawingCompatMode::Libass;
		default:
			return kDefaultAssDrawingCompatMode;
	}
}

PathData ParseAss(std::string_view ass_shape, AssDrawingCompatMode compat_mode, AssDrawingPathMode path_mode) {
	auto tokens = TokenizeCompatibleDrawingTokens(ass_shape, compat_mode);
	return BuildPathFromTokens(tokens, GetDrawingCompatBehavior(compat_mode), GetPathBuildBehavior(path_mode));
}

Point TransformPoint(Point point, Matrix3x2 const& matrix) {
	return {
		point.x * matrix.m11 + point.y * matrix.m21 + matrix.dx,
		point.x * matrix.m12 + point.y * matrix.m22 + matrix.dy,
	};
}

PathData TransformPath(PathData path, Matrix3x2 const& matrix) {
	for (auto& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
			case PathVerb::LineTo:
				command.p1 = TransformPoint(command.p1, matrix);
				break;
			case PathVerb::QuadTo:
				command.p1 = TransformPoint(command.p1, matrix);
				command.p2 = TransformPoint(command.p2, matrix);
				break;
			case PathVerb::ConicTo:
				command.p1 = TransformPoint(command.p1, matrix);
				command.p2 = TransformPoint(command.p2, matrix);
				break;
			case PathVerb::CubicTo:
				command.p1 = TransformPoint(command.p1, matrix);
				command.p2 = TransformPoint(command.p2, matrix);
				command.p3 = TransformPoint(command.p3, matrix);
				break;
			case PathVerb::Close:
				break;
		}
	}
	return path;
}

std::vector<Lexeme> LexDrawing(std::string_view text) {
	std::vector<Lexeme> lexemes;
	HighlightState state;

	std::string null_terminated_text(text);
	char const* cursor = null_terminated_text.c_str();
	char const* end = cursor + null_terminated_text.size();

	while (cursor < end) {
		if (IsWhitespace(*cursor)) {
			char const* whitespace_start = cursor;
			while (cursor < end && IsWhitespace(*cursor))
				++cursor;
			AddLexeme(lexemes, static_cast<std::size_t>(cursor - whitespace_start), LexemeType::Normal);
		}
		else if (IsDrawingCommand(static_cast<unsigned char>(*cursor))) {
			cursor = LexDrawingCommand(lexemes, cursor, end, state);
		}
		else {
			char const* number_end = nullptr;
			if (ReadNumber(cursor, end, number_end)) {
				AddLexeme(lexemes, static_cast<std::size_t>(number_end - cursor), LexemeType::Error);
				cursor = number_end;
			}
			else {
				AddLexeme(lexemes, 1, LexemeType::Error);
				++cursor;
			}
		}
	}

	return lexemes;
}

} // namespace drawing
} // namespace ass
} // namespace agi
