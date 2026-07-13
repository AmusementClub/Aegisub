// Copyright (c) 2026 Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include <main.h>

#include <libaegisub/ass/drawing.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace agi::ass::drawing;

namespace {

void ExpectPoint(Point const& point, double x, double y) {
	EXPECT_DOUBLE_EQ(x, point.x);
	EXPECT_DOUBLE_EQ(y, point.y);
}

void ExpectLexeme(Lexeme const& lexeme, LexemeType type, std::size_t length) {
	EXPECT_EQ(type, lexeme.type);
	EXPECT_EQ(length, lexeme.length);
}

void ExpectRect(Rect const& rect, double x, double y, double width, double height) {
	EXPECT_DOUBLE_EQ(x, rect.x);
	EXPECT_DOUBLE_EQ(y, rect.y);
	EXPECT_DOUBLE_EQ(width, rect.width);
	EXPECT_DOUBLE_EQ(height, rect.height);
}

void ExpectSkiaContains(PathData const& path, double x, double y, bool expected) {
	bool contains = !expected;
	ASSERT_TRUE(TryDrawingContainsPoint(path, x, y, contains));
	EXPECT_EQ(expected, contains);
}

}

TEST(lagi_ass_drawing, lexes_coordinates_and_cubic_endpoints) {
	auto lexemes = LexDrawing("m +1.5 -2E-3 b 0 0 0 100 100 0");

	ASSERT_EQ(18u, lexemes.size());
	ExpectLexeme(lexemes[0], LexemeType::Command, 1u);
	ExpectLexeme(lexemes[1], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[2], LexemeType::X, 4u);
	ExpectLexeme(lexemes[3], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[4], LexemeType::Y, 5u);
	ExpectLexeme(lexemes[5], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[6], LexemeType::Command, 1u);
	ExpectLexeme(lexemes[7], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[8], LexemeType::X, 1u);
	ExpectLexeme(lexemes[9], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[10], LexemeType::Y, 1u);
	ExpectLexeme(lexemes[11], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[12], LexemeType::X, 1u);
	ExpectLexeme(lexemes[13], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[14], LexemeType::Y, 3u);
	ExpectLexeme(lexemes[15], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[16], LexemeType::EndpointX, 4u);
	ExpectLexeme(lexemes[17], LexemeType::EndpointY, 1u);
}

TEST(lagi_ass_drawing, lexes_missing_root_command_as_error) {
	auto lexemes = LexDrawing("l 100 100");

	ASSERT_EQ(5u, lexemes.size());
	ExpectLexeme(lexemes[0], LexemeType::Error, 1u);
	ExpectLexeme(lexemes[1], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[2], LexemeType::Error, 3u);
	ExpectLexeme(lexemes[3], LexemeType::Normal, 1u);
	ExpectLexeme(lexemes[4], LexemeType::Error, 3u);
}

TEST(lagi_ass_drawing, parses_line_shape_to_path) {
	auto path = ParseAss("m 0 0 l 10 10");

	ASSERT_EQ(3u, path.commands.size());
	EXPECT_EQ(PathVerb::MoveTo, path.commands[0].verb);
	ExpectPoint(path.commands[0].p1, 0.0, 0.0);
	EXPECT_EQ(PathVerb::LineTo, path.commands[1].verb);
	ExpectPoint(path.commands[1].p1, 10.0, 10.0);
	EXPECT_EQ(PathVerb::Close, path.commands[2].verb);
}

TEST(lagi_ass_drawing, preserves_open_move_anchor_when_requested) {
	auto path = ParseAssOpen("m 5 6");

	ASSERT_EQ(1u, path.commands.size());
	EXPECT_EQ(PathVerb::MoveTo, path.commands[0].verb);
	ExpectPoint(path.commands[0].p1, 5.0, 6.0);
}

TEST(lagi_ass_drawing, parses_cubic_bezier) {
	auto path = ParseAssOpen("m 0 0 b 10 0 10 20 20 20");

	ASSERT_EQ(2u, path.commands.size());
	EXPECT_EQ(PathVerb::MoveTo, path.commands[0].verb);
	EXPECT_EQ(PathVerb::CubicTo, path.commands[1].verb);
	ExpectPoint(path.commands[1].p1, 10.0, 0.0);
	ExpectPoint(path.commands[1].p2, 10.0, 20.0);
	ExpectPoint(path.commands[1].p3, 20.0, 20.0);
}

TEST(lagi_ass_drawing, parses_decimal_numbers_without_locale_or_range_hazards) {
	auto path = ParseAssOpen("m +1.5 -2E-2", AssDrawingCompatMode::VsFilter);
	ASSERT_EQ(1u, path.commands.size());
	ExpectPoint(path.commands.front().p1, 1.5, -1.0 / 64.0);

	EXPECT_TRUE(ParseAssOpen("m 1e999 0 l 10 10").commands.empty());
	EXPECT_TRUE(ParseAssOpen("m 1e308 0").commands.empty());

	PathData extreme;
	extreme.commands.push_back({PathVerb::MoveTo, {std::numeric_limits<double>::max(), 0.0}, {}, {}});
	auto serialized = SerializeAss(extreme);
	EXPECT_EQ(std::string::npos, serialized.find_first_of("eE"));
	EXPECT_FALSE(serialized.empty());
}

TEST(lagi_ass_drawing, transforms_path) {
	auto path = ParseAssOpen("m 1 2 l 3 4");
	auto transformed = TransformPath(path, {2.0, 0.0, 0.0, 3.0, 5.0, 7.0});

	ASSERT_EQ(2u, transformed.commands.size());
	ExpectPoint(transformed.commands[0].p1, 7.0, 13.0);
	ExpectPoint(transformed.commands[1].p1, 11.0, 19.0);
}

TEST(lagi_ass_drawing, measures_path_length_and_positions) {
	auto path = ParseAssOpen("m 0 0 l 3 4 l 6 8");
	Point point;
	Point tangent;

	EXPECT_DOUBLE_EQ(10.0, PathLength(path));
	EXPECT_DOUBLE_EQ(0.5, PercentAtLength(path, 5.0));
	ASSERT_TRUE(TryGetPositionAtPercent(path, 0.5, point, tangent));
	ExpectPoint(point, 3.0, 4.0);
	EXPECT_NEAR(0.0, tangent.x * 8.0 - tangent.y * 6.0, 1e-12);
	EXPECT_GT(tangent.x * 6.0 + tangent.y * 8.0, 0.0);
	ASSERT_TRUE(TryGetPositionAtLength(ParseAssOpen("m 0 0 l 10 0 l 10 10"), 15.0, point, tangent));
	ExpectPoint(point, 10.0, 5.0);
	ExpectPoint(tangent, 0.0, 10.0);
}

TEST(lagi_ass_drawing, measures_cubic_position_by_arc_length) {
	auto path = ParseAssOpen("m 0 0 b 0 10 10 10 10 0");
	Point point;
	Point tangent;

	EXPECT_NEAR(20.0, PathLength(path), 0.1);
	ASSERT_TRUE(TryGetPositionAtPercent(path, 0.5, point, tangent));
	EXPECT_NEAR(5.0, point.x, 0.01);
	EXPECT_NEAR(7.5, point.y, 0.01);
}

TEST(lagi_ass_drawing, separates_modern_arc_length_and_legacy_qt_percent_mappings) {
	auto path = ParseAssOpen("m 0 0 b 0 10 20 10 30 0");
	Point modern_point;
	Point modern_tangent;
	Point legacy_point;
	Point legacy_tangent;

	ASSERT_TRUE(TryGetPositionAtPercent(path, 0.25, modern_point, modern_tangent));
	ASSERT_TRUE(TryGetLegacyPositionAtPercent(path, 0.25, legacy_point, legacy_tangent));
	// A single Qt curve maps global percent directly to its Bezier t.
	ExpectPoint(legacy_point, 3.28125, 5.625);
	EXPECT_GT(std::hypot(modern_point.x - legacy_point.x, modern_point.y - legacy_point.y), 0.01);

	double total = PathLength(path);
	double distance = total * 0.25;
	double legacy_percent = LegacyPercentAtLength(path, distance);
	EXPECT_GT(std::abs(legacy_percent - PercentAtLength(path, distance)), 1e-4);

	Point point_at_length;
	Point tangent_at_length;
	ASSERT_TRUE(TryGetPositionAtLength(path, distance, point_at_length, tangent_at_length));
	ASSERT_TRUE(TryGetLegacyPositionAtPercent(path, legacy_percent, legacy_point, legacy_tangent));
	EXPECT_NEAR(point_at_length.x, legacy_point.x, 1e-7);
	EXPECT_NEAR(point_at_length.y, legacy_point.y, 1e-7);

	legacy_point = {9.0, 9.0};
	legacy_tangent = {9.0, 9.0};
	EXPECT_FALSE(TryGetLegacyPositionAtPercent(path, -0.01, legacy_point, legacy_tangent));
	ExpectPoint(legacy_point, 0.0, 0.0);
	ExpectPoint(legacy_tangent, 0.0, 0.0);
	EXPECT_FALSE(TryGetLegacyPositionAtPercent(path, 1.01, legacy_point, legacy_tangent));
	ExpectPoint(legacy_point, 0.0, 0.0);

	EXPECT_DOUBLE_EQ(0.0, LegacyPercentAtLength(path, -1.0));
	EXPECT_DOUBLE_EQ(1.0, LegacyPercentAtLength(path, std::numeric_limits<double>::infinity()));
	EXPECT_DOUBLE_EQ(0.0, LegacyPercentAtLength(path, std::numeric_limits<double>::quiet_NaN()));
}

TEST(lagi_ass_drawing, raw_geometry_preserves_precision_curves_close_and_contours) {
	constexpr double span = 2.004;
	constexpr double conic_weight = 0.75;
	Point const first {0.001, 0.002};
	Point const second {first.x + span, first.y};
	Point const third {second.x, second.y + span};

	PathData path;
	path.commands.push_back({PathVerb::MoveTo, first, {}, {}});
	path.commands.push_back({PathVerb::QuadTo, {(first.x + second.x) * 0.5, first.y}, second, {}});
	path.commands.push_back({PathVerb::ConicTo, {second.x, (second.y + third.y) * 0.5}, third, {}, conic_weight});
	path.commands.push_back({PathVerb::Close, {}, {}, {}});
	path.commands.push_back({PathVerb::MoveTo, {10.0, 10.0}, {}, {}});
	path.commands.push_back({PathVerb::LineTo, {13.0, 14.0}, {}, {}});

	double expected_first_contour_length = span * 2.0 + std::hypot(span, span);
	double total_length = PathLength(path);
	EXPECT_NEAR(expected_first_contour_length + 5.0, total_length, 1e-5);

	Point point;
	Point tangent;
	ASSERT_TRUE(TryGetPositionAtLength(path, total_length - 3.0, point, tangent));
	EXPECT_NEAR(11.2, point.x, 1e-9);
	EXPECT_NEAR(11.6, point.y, 1e-9);

	Rect bounds;
	ASSERT_TRUE(TryGetBounds(path, bounds));
	EXPECT_DOUBLE_EQ(first.x, bounds.x);
	EXPECT_DOUBLE_EQ(first.y, bounds.y);
	EXPECT_DOUBLE_EQ(13.0 - first.x, bounds.width);
	EXPECT_DOUBLE_EQ(14.0 - first.y, bounds.height);

	auto flattened = FlattenPath(path, 0.01);
	ASSERT_FALSE(flattened.commands.empty());
	ExpectPoint(flattened.commands.front().p1, first.x, first.y);
	EXPECT_EQ(1, std::count_if(flattened.commands.begin(), flattened.commands.end(), [](PathCommand const& command) {
		return command.verb == PathVerb::Close;
	}));
	for (auto const& command : flattened.commands)
		EXPECT_TRUE(command.verb == PathVerb::MoveTo || command.verb == PathVerb::LineTo || command.verb == PathVerb::Close);

	auto reversed = ReversePath(path);
	ASSERT_EQ(6u, reversed.commands.size());
	EXPECT_EQ(PathVerb::MoveTo, reversed.commands[0].verb);
	ExpectPoint(reversed.commands[0].p1, third.x, third.y);
	EXPECT_EQ(PathVerb::ConicTo, reversed.commands[1].verb);
	ExpectPoint(reversed.commands[1].p2, second.x, second.y);
	EXPECT_DOUBLE_EQ(conic_weight, reversed.commands[1].weight);
	EXPECT_EQ(PathVerb::QuadTo, reversed.commands[2].verb);
	ExpectPoint(reversed.commands[2].p2, first.x, first.y);
	EXPECT_EQ(PathVerb::Close, reversed.commands[3].verb);
	EXPECT_EQ(PathVerb::MoveTo, reversed.commands[4].verb);
	ExpectPoint(reversed.commands[4].p1, 13.0, 14.0);
	EXPECT_EQ(PathVerb::LineTo, reversed.commands[5].verb);
	ExpectPoint(reversed.commands[5].p1, 10.0, 10.0);
	EXPECT_NEAR(total_length, PathLength(reversed), 1e-5);
}

TEST(lagi_ass_drawing, computes_filled_area_and_centroid) {
	auto path = ParseAss("m 0 0 l 10 0 l 10 10 l 0 10");
	double signed_area;
	Point centroid;

	ASSERT_TRUE(TryGetSignedAreaAndCentroid(path, signed_area, centroid));
	EXPECT_DOUBLE_EQ(100.0, signed_area);
	ExpectPoint(centroid, 5.0, 5.0);
}

TEST(lagi_ass_drawing, builds_basic_shapes_without_backend_dependencies) {
	Rect bounds;

	EXPECT_EQ("m 0 0 l 10 0 10 10 0 10", SerializeAssFilled(MakeRect(0.0, 0.0, 10.0, 10.0)));
	ASSERT_TRUE(TryGetBounds(MakeEllipse(0.0, 0.0, 20.0, 10.0), bounds));
	ExpectRect(bounds, 0.0, 0.0, 20.0, 10.0);
	ASSERT_TRUE(TryGetBounds(MakeRoundedRect(0.0, 0.0, 20.0, 10.0, 3.0, 2.0), bounds));
	ExpectRect(bounds, 0.0, 0.0, 20.0, 10.0);

	double rect_area;
	double ellipse_area;
	Point centroid;
	ASSERT_TRUE(TryGetSignedAreaAndCentroid(MakeRect(0.0, 0.0, 20.0, 10.0), rect_area, centroid));
	ASSERT_TRUE(TryGetSignedAreaAndCentroid(MakeEllipse(0.0, 0.0, 20.0, 10.0), ellipse_area, centroid));
	EXPECT_GT(rect_area, 0.0);
	EXPECT_GT(ellipse_area, 0.0);

	EXPECT_TRUE(MakeRect(std::numeric_limits<double>::infinity(), 0.0, 20.0, 10.0).empty());
	EXPECT_TRUE(MakeRoundedRect(0.0, 0.0, 20.0, 10.0,
		std::numeric_limits<double>::quiet_NaN(), 2.0).empty());
}

TEST(lagi_ass_drawing, appends_ellipse_arc_commands) {
	PathData path;
	AppendArcMoveTo(path, 0.0, 0.0, 20.0, 20.0, 90.0);
	EXPECT_EQ("m 10 0", SerializeAss(path));

	auto continued = ParseAssOpen("m 20 10");
	AppendArcTo(continued, 0.0, 0.0, 20.0, 20.0, 90.0, 90.0);
	Rect bounds;
	ASSERT_TRUE(TryGetBounds(continued, bounds));
	ExpectRect(bounds, 0.0, 0.0, 20.0, 10.0);
}

TEST(lagi_ass_drawing, arc_angles_wrap_and_signed_dimensions_mirror_geometry) {
	PathData path;
	AppendArcMoveTo(path, 0.0, 0.0, 20.0, 10.0, 450.0);
	ASSERT_EQ(1u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 10.0, 0.0);

	path = {};
	AppendArcMoveTo(path, 20.0, 0.0, -20.0, 10.0, 0.0);
	ASSERT_EQ(1u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 0.0, 5.0);
	AppendArcTo(path, 20.0, 0.0, -20.0, 10.0, 0.0, 90.0);
	ASSERT_EQ(PathVerb::CubicTo, path.commands.back().verb);
	ExpectPoint(path.commands.back().p3, 10.0, 0.0);

	path = {};
	AppendArcMoveTo(path, 0.0, 10.0, 20.0, -10.0, 90.0);
	ASSERT_EQ(1u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 10.0, 10.0);
}

TEST(lagi_ass_drawing, arc_zero_sweep_preserves_connection_without_curve) {
	PathData empty;
	AppendArcTo(empty, 0.0, 0.0, 20.0, 10.0, 0.0, 0.0);
	ASSERT_EQ(2u, empty.commands.size());
	EXPECT_EQ(PathVerb::MoveTo, empty.commands[0].verb);
	ExpectPoint(empty.commands[0].p1, 0.0, 0.0);
	EXPECT_EQ(PathVerb::LineTo, empty.commands[1].verb);
	ExpectPoint(empty.commands[1].p1, 20.0, 5.0);

	auto matching = ParseAssOpen("m 20 5");
	AppendArcTo(matching, 0.0, 0.0, 20.0, 10.0, 0.0, 0.0);
	ASSERT_EQ(1u, matching.commands.size());

	auto mismatched = ParseAssOpen("m 1 1");
	AppendArcTo(mismatched, 0.0, 0.0, 20.0, 10.0, 0.0, 0.0);
	ASSERT_EQ(2u, mismatched.commands.size());
	EXPECT_EQ(PathVerb::LineTo, mismatched.commands.back().verb);
}

TEST(lagi_ass_drawing, arc_sweep_is_clamped_to_one_turn_in_both_directions) {
	for (double sweep : {450.0, -450.0}) {
		PathData path;
		AppendArcTo(path, 0.0, 0.0, 20.0, 10.0, 0.0, sweep);
		ASSERT_EQ(6u, path.commands.size());
		EXPECT_EQ(PathVerb::MoveTo, path.commands[0].verb);
		EXPECT_EQ(PathVerb::LineTo, path.commands[1].verb);
		for (std::size_t index = 2; index < path.commands.size(); ++index)
			EXPECT_EQ(PathVerb::CubicTo, path.commands[index].verb);
		ExpectPoint(path.commands.back().p3, 20.0, 5.0);
	}
}

TEST(lagi_ass_drawing, arc_after_close_connects_from_subpath_start) {
	auto path = MakeRect(1.0, 1.0, 1.0, 1.0);
	ASSERT_EQ(PathVerb::Close, path.commands.back().verb);
	AppendArcTo(path, 0.0, 0.0, 20.0, 10.0, 0.0, 90.0);
	ASSERT_GE(path.commands.size(), 7u);
	EXPECT_EQ(PathVerb::LineTo, path.commands[5].verb);
	ExpectPoint(path.commands[5].p1, 20.0, 5.0);
	EXPECT_EQ(PathVerb::CubicTo, path.commands[6].verb);
	ExpectPoint(path.commands[6].p3, 10.0, 0.0);
}

TEST(lagi_ass_drawing, computes_exact_cubic_bounds) {
	auto path = ParseAssOpen("m 0 0 b 0 10 10 10 10 0");
	Rect bounds;

	ASSERT_TRUE(TryGetBounds(path, bounds));
	ExpectRect(bounds, 0.0, 0.0, 10.0, 7.5);
}

TEST(lagi_ass_drawing, empty_bounds_are_reported) {
	Rect bounds;

	EXPECT_FALSE(TryGetBounds(ParseAssOpen(""), bounds));
}

TEST(lagi_ass_drawing, serializes_open_path) {
	auto path = ParseAssOpen("m 0 0 l 10 10");

	EXPECT_EQ("m 0 0 l 10 10", SerializeAss(path));
}

TEST(lagi_ass_drawing, serializes_explicit_close_as_line_for_open_path) {
	auto path = ParseAss("m 0 0 l 10 0 l 10 10");

	EXPECT_EQ("m 0 0 l 10 0 10 10 0 0", SerializeAss(path));
}

TEST(lagi_ass_drawing, serializes_filled_path_with_implicit_close) {
	auto path = ParseAss("m 0 0 l 10 0 l 10 10");

	EXPECT_EQ("m 0 0 l 10 0 10 10", SerializeAssFilled(path));
}

TEST(lagi_ass_drawing, checked_filled_serializers_reject_even_odd_paths) {
	auto path = MakeRect(0.0, 0.0, 10.0, 10.0);
	std::string output = "stale";

	ASSERT_TRUE(TrySerializeAssFilled(path, output));
	EXPECT_EQ("m 0 0 l 10 0 10 10 0 10", output);
	ASSERT_TRUE(TrySerializeAssCompactFilled(path, output));
	EXPECT_EQ("m 0 0 l 10 0 10 10 0 10", output);

	path.winding_fill = false;
	EXPECT_FALSE(TrySerializeAssFilled(path, output));
	EXPECT_TRUE(output.empty());
	output = "stale";
	EXPECT_FALSE(TrySerializeAssCompactFilled(path, output));
	EXPECT_TRUE(output.empty());
}

TEST(lagi_ass_drawing, compacts_filled_path_text_when_opted_in) {
	auto shape = "m 10 10 l 0 10 0 0 10 0 10 10";
	auto compact = CompactAss(shape);
	double original_area;
	double compact_area;
	Point centroid;

	EXPECT_EQ("m 0 0 l 10 0 10 10 0 10", compact);
	EXPECT_LT(compact.size(), SerializeAssFilled(ParseAss(shape)).size());
	ASSERT_TRUE(TryGetSignedAreaAndCentroid(ParseAss(shape), original_area, centroid));
	ASSERT_TRUE(TryGetSignedAreaAndCentroid(ParseAss(compact), compact_area, centroid));
	EXPECT_DOUBLE_EQ(original_area, compact_area);
}

TEST(lagi_ass_drawing, compacts_mixed_closed_contours_without_reversing_curves) {
	auto shape = "m 0 0 l 1 1 b 10 9 13 9 2 2 l 100000 100000";
	auto compact = CompactAss(shape);

	EXPECT_EQ("m 1 1 b 10 9 13 9 2 2 l 100000 100000 0 0", compact);
	EXPECT_LT(compact.size(), SerializeAssFilled(ParseAss(shape)).size());
}

TEST(lagi_ass_drawing, resets_pen_after_implicit_close_when_path_continues) {
	PathData path;
	path.commands.push_back({PathVerb::MoveTo, {0.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::LineTo, {10.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::Close, {}, {}, {}});
	path.commands.push_back({PathVerb::LineTo, {0.0, 10.0}, {}, {}});

	EXPECT_EQ("m 0 0 l 10 0 m 0 0 l 0 10", SerializeAssFilled(path));
}

TEST(lagi_ass_drawing, drops_degenerate_segments_and_merges_collinear_lines) {
	PathData path;
	path.commands.push_back({PathVerb::MoveTo, {0.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::LineTo, {10.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::LineTo, {20.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::LineTo, {20.0, 0.0}, {}, {}});

	EXPECT_EQ("m 0 0 l 20 0", SerializeAss(path));
}

TEST(lagi_ass_drawing, lowers_quadratic_curves_to_d6_cubics) {
	PathData path;
	path.commands.push_back({PathVerb::MoveTo, {0.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::QuadTo, {10.0, 0.0}, {10.0, 10.0}, {}});

	EXPECT_EQ("m 0 0 b 6.672 0 10 3.33 10 10", SerializeAss(path));
}

TEST(lagi_ass_drawing, emits_short_decimals_with_identical_renderer_d6_values) {
	struct Case {
		int d6;
		char const *text;
	};
	constexpr Case cases[] = {
		{1, "0.02"},
		{-1, "-0.02"},
		{2, "0.032"},
		{5, "0.08"},
		{43, "0.672"},
		{63, "0.99"},
		{65, "1.02"},
		{427, "6.672"},
		{-427, "-6.672"},
	};

	for (auto const& test : cases) {
		PathData path;
		path.commands.push_back({PathVerb::MoveTo, {test.d6 / 64.0, 0.0}, {}, {}});
		std::string expected = std::string("m ") + test.text + " 0";
		EXPECT_EQ(expected, SerializeAss(path));

		auto vsfilter = ParseAssOpen(expected, AssDrawingCompatMode::VsFilter);
		auto libass = ParseAssOpen(expected, AssDrawingCompatMode::Libass);
		ASSERT_EQ(1u, vsfilter.commands.size());
		ASSERT_EQ(1u, libass.commands.size());
		EXPECT_DOUBLE_EQ(test.d6 / 64.0, vsfilter.commands.front().p1.x);
		EXPECT_DOUBLE_EQ(test.d6 / 64.0, libass.commands.front().p1.x);
	}
}

TEST(lagi_ass_drawing, renderer_compatible_decimals_cover_all_d6_residues) {
	for (int d6 = -4096; d6 <= 4096; ++d6) {
		SCOPED_TRACE(d6);
		PathData path;
		path.commands.push_back({PathVerb::MoveTo, {d6 / 64.0, 0.0}, {}, {}});
		auto text = SerializeAss(path);
		EXPECT_EQ(std::string::npos, text.find(','));
		EXPECT_EQ(std::string::npos, text.find_first_of("eE"));

		auto vsfilter = ParseAssOpen(text, AssDrawingCompatMode::VsFilter);
		auto libass = ParseAssOpen(text, AssDrawingCompatMode::Libass);
		ASSERT_EQ(1u, vsfilter.commands.size());
		ASSERT_EQ(1u, libass.commands.size());
		EXPECT_DOUBLE_EQ(d6 / 64.0, vsfilter.commands.front().p1.x);
		EXPECT_DOUBLE_EQ(d6 / 64.0, libass.commands.front().p1.x);
	}
}

TEST(lagi_ass_drawing, lowers_collinear_cubics_to_lines) {
	PathData path;
	path.commands.push_back({PathVerb::MoveTo, {0.0, 0.0}, {}, {}});
	path.commands.push_back({PathVerb::CubicTo, {5.0, 0.0}, {10.0, 0.0}, {15.0, 0.0}});

	EXPECT_EQ("m 0 0 l 15 0", SerializeAss(path));
}

TEST(lagi_ass_drawing, flattens_cubic_curves_to_lines) {
	auto flat = FlattenPath(ParseAssOpen("m 0 0 b 0 10 10 10 10 0"), 100.0);

	EXPECT_EQ("m 0 0 l 10 0", SerializeAss(flat));
	for (auto const& command : flat.commands)
		EXPECT_NE(PathVerb::CubicTo, command.verb);
}

TEST(lagi_ass_drawing, reverses_lines_and_cubic_curves) {
	auto path = ParseAssOpen("m 0 0 l 10 0 b 10 10 20 10 20 0");

	EXPECT_EQ("m 20 0 b 20 10 10 10 10 0 l 0 0", SerializeAss(ReversePath(path)));
}

TEST(lagi_ass_drawing, tight_bounds_include_curve_extrema) {
	Rect bounds;
	ASSERT_TRUE(TryGetBounds(ParseAssOpen("m 0 0 b 0 10 10 10 10 0"), bounds));
	ExpectRect(bounds, 0.0, 0.0, 10.0, 7.5);
}

TEST(lagi_ass_drawing, control_point_bounds_match_legacy_shape_semantics) {
	Rect bounds;
	ASSERT_TRUE(TryGetControlPointBounds(ParseAss("m 0 0 b 0 10 10 10 10 0"), bounds));
	// Legacy shape_bouding uses the control-point rectangle, not curve extrema.
	ExpectRect(bounds, 0.0, 0.0, 10.0, 10.0);

	ASSERT_FALSE(TryGetControlPointBounds({}, bounds));
}


TEST(lagi_ass_drawing, skia_backend_reports_unavailable_without_feature) {
	if (DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is enabled";

	bool contains = true;
	EXPECT_FALSE(TryDrawingContainsPoint(ParseAss("m 0 0 l 10 0 l 10 10 l 0 10"), 5.0, 5.0, contains));
	EXPECT_FALSE(contains);

	PathData result;
	EXPECT_FALSE(TryDrawingBoolean(ParseAss("m 0 0 l 10 0 l 10 10 l 0 10"),
		ParseAss("m 5 5 l 15 5 l 15 15 l 5 15"),
		DrawingBooleanOp::Union,
		result));
}

TEST(lagi_ass_drawing, skia_backend_evaluates_filled_path_queries) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	bool contains = false;
	auto rect = ParseAss("m 0 0 l 10 0 l 10 10 l 0 10");

	ASSERT_TRUE(TryDrawingContainsPoint(rect, 5.0, 5.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(rect, 15.0, 5.0, contains));
	EXPECT_FALSE(contains);
	ASSERT_TRUE(TryDrawingContainsRect(rect, 2.0, 2.0, 2.0, 2.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsRect(rect, 8.0, 8.0, 4.0, 4.0, contains));
	EXPECT_FALSE(contains);
}

TEST(lagi_ass_drawing, skia_backend_evaluates_boolean_operations) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	auto lhs = ParseAss("m 0 0 l 10 0 l 10 10 l 0 10");
	auto rhs = ParseAss("m 5 5 l 15 5 l 15 15 l 5 15");
	PathData result;
	bool contains = false;

	ASSERT_TRUE(TryDrawingBoolean(lhs, rhs, DrawingBooleanOp::Union, result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 12.0, 12.0, contains));
	EXPECT_TRUE(contains);
	// Boolean ASS export should keep a non-empty filled serialization after closed-figure normalization.
	EXPECT_FALSE(SerializeAssFilled(result).empty());

	ASSERT_TRUE(TryDrawingBoolean(lhs, rhs, DrawingBooleanOp::Intersect, result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 7.0, 7.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(result, 2.0, 2.0, contains));
	EXPECT_FALSE(contains);

	ASSERT_TRUE(TryDrawingBoolean(lhs, rhs, DrawingBooleanOp::Subtract, result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 2.0, 2.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(result, 7.0, 7.0, contains));
	EXPECT_FALSE(contains);
	auto subtract_roundtrip = ParseAss(SerializeAssFilled(result));
	ExpectSkiaContains(subtract_roundtrip, 2.0, 2.0, true);
	ExpectSkiaContains(subtract_roundtrip, 7.0, 7.0, false);

	ASSERT_TRUE(TryDrawingBoolean(lhs, rhs, DrawingBooleanOp::Xor, result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 2.0, 2.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(result, 7.0, 7.0, contains));
	EXPECT_FALSE(contains);
	auto xor_roundtrip = ParseAss(SerializeAssFilled(result));
	ExpectSkiaContains(xor_roundtrip, 2.0, 2.0, true);
	ExpectSkiaContains(xor_roundtrip, 7.0, 7.0, false);
}

TEST(lagi_ass_drawing, skia_backend_generates_outlines) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	PathData result;
	bool contains = false;

	ASSERT_TRUE(TryDrawingOutline(ParseAssOpen("m 0 0 l 10 0"), 2.0, DrawingStrokeCap::Flat, DrawingStrokeJoin::Bevel, result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 5.0, 0.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(result, 5.0, 2.0, contains));
	EXPECT_FALSE(contains);

	ASSERT_TRUE(TryDrawingPatternOutline(ParseAssOpen("m 0 0 l 20 0"),
		2.0,
		DrawingStrokeCap::Flat,
		DrawingStrokeJoin::Bevel,
		2.5,
		2.5,
		0.0,
		result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 2.0, 0.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(result, 7.0, 0.0, contains));
	EXPECT_FALSE(contains);

	ASSERT_TRUE(TryDrawingOutline(MakeRect(0.0, 0.0, 20.0, 20.0),
		2.0, DrawingStrokeCap::Square, DrawingStrokeJoin::Miter, result));
	ExpectSkiaContains(result, 0.5, 10.0, true);
	ExpectSkiaContains(result, 10.0, 10.0, false);
	auto outline_roundtrip = ParseAss(SerializeAssFilled(result));
	ExpectSkiaContains(outline_roundtrip, 0.5, 10.0, true);
	ExpectSkiaContains(outline_roundtrip, 10.0, 10.0, false);
}

TEST(lagi_ass_drawing, skia_backend_continues_from_closed_contour_start) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	PathData source;
	source.commands.push_back({PathVerb::MoveTo, {10.0, 10.0}, {}, {}});
	source.commands.push_back({PathVerb::LineTo, {20.0, 10.0}, {}, {}});
	source.commands.push_back({PathVerb::Close, {}, {}, {}});
	source.commands.push_back({PathVerb::LineTo, {10.0, 20.0}, {}, {}});

	PathData outline;
	ASSERT_TRUE(TryDrawingOutline(source, 2.0, DrawingStrokeCap::Flat, DrawingStrokeJoin::Bevel, outline));
	ExpectSkiaContains(outline, 10.0, 15.0, true);
	ExpectSkiaContains(outline, 7.5, 15.0, false);
}

TEST(lagi_ass_drawing, skia_backend_handles_holes_and_multiple_contours) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	auto donut = ParseAss("m 0 0 l 40 0 40 40 0 40 m 10 10 l 10 30 30 30 30 10");
	ExpectSkiaContains(donut, 5.0, 5.0, true);
	ExpectSkiaContains(donut, 20.0, 20.0, false);

	PathData result;
	ASSERT_TRUE(TryDrawingBoolean(donut, ParseAss("m 12 12 l 28 12 28 28 12 28"), DrawingBooleanOp::Union, result));
	ExpectSkiaContains(result, 20.0, 20.0, true);
	ExpectSkiaContains(result, 5.0, 5.0, true);

	ASSERT_TRUE(TryDrawingBoolean(ParseAss("m 0 0 l 40 0 40 40 0 40"),
		ParseAss("m 10 10 l 30 10 30 30 10 30"),
		DrawingBooleanOp::Subtract,
		result));
	ExpectSkiaContains(result, 5.0, 5.0, true);
	ExpectSkiaContains(result, 20.0, 20.0, false);
}

TEST(lagi_ass_drawing, skia_backend_keeps_isolated_contours_semantically_stable) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	PathData result;
	auto isolated = ParseAss("m 0 0 l 10 0 10 10 0 10 m 100 0 l 110 0 110 10 100 10");
	ASSERT_TRUE(TryDrawingBoolean(isolated, ParseAss("m 200 0 l 210 0 210 10 200 10"), DrawingBooleanOp::Union, result));

	ExpectSkiaContains(result, 5.0, 5.0, true);
	ExpectSkiaContains(result, 105.0, 5.0, true);
	ExpectSkiaContains(result, 205.0, 5.0, true);
	ExpectSkiaContains(result, 50.0, 5.0, false);
	ExpectSkiaContains(result, 150.0, 5.0, false);
}

TEST(lagi_ass_drawing, skia_backend_handles_self_intersections_and_large_coordinates) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	PathData result;
	auto bowtie = ParseAss("m 0 0 l 20 20 0 20 20 0");
	auto large = ParseAss("m 1000000 1000000 l 1000100 1000000 1000100 1000100 1000000 1000100");
	ASSERT_TRUE(TryDrawingBoolean(bowtie, large, DrawingBooleanOp::Union, result));
	ExpectSkiaContains(result, 1000050.0, 1000050.0, true);
	ExpectSkiaContains(result, 500000.0, 500000.0, false);

	Rect bounds;
	ASSERT_TRUE(TryGetBounds(result, bounds));
	EXPECT_GE(bounds.width, 1000100.0);
	EXPECT_GE(bounds.height, 1000100.0);
}

TEST(lagi_ass_drawing, skia_backend_preserves_small_d6_geometry_at_large_origins) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	constexpr double base = 10000000.0 + 1.0 / 64.0;
	PathData result;
	ASSERT_TRUE(TryDrawingBoolean(
		MakeRect(base, base, 1.0, 1.0),
		MakeRect(base + 0.5, base, 1.0, 1.0),
		DrawingBooleanOp::Union,
		result));
	ExpectSkiaContains(result, base + 0.25, base + 0.5, true);
	ExpectSkiaContains(result, base + 1.25, base + 0.5, true);
	ExpectSkiaContains(result, base + 1.75, base + 0.5, false);

	Rect bounds;
	ASSERT_TRUE(TryGetBounds(result, bounds));
	EXPECT_DOUBLE_EQ(base, bounds.x);
	EXPECT_DOUBLE_EQ(base, bounds.y);
	EXPECT_DOUBLE_EQ(1.5, bounds.width);
	EXPECT_DOUBLE_EQ(1.0, bounds.height);

	ASSERT_TRUE(TryDrawingOutline(
		MakeRect(base, base, 1.0, 1.0),
		0.25,
		DrawingStrokeCap::Square,
		DrawingStrokeJoin::Miter,
		result));
	ExpectSkiaContains(result, base - 0.0625, base + 0.5, true);
	ExpectSkiaContains(result, base + 0.5, base + 0.5, false);

	bool contains = false;
	ASSERT_TRUE(TryDrawingContainsRect(MakeRect(base, base, 1.0, 1.0),
		base + 0.125, base + 0.125, 0.25, 0.25, contains));
	EXPECT_TRUE(contains);
}

TEST(lagi_ass_drawing, skia_backend_handles_degenerate_outline_parameters) {
	if (!DrawingSkiaBackendAvailable())
		GTEST_SKIP() << "drawing Skia backend is not enabled";

	PathData result;
	ASSERT_TRUE(TryDrawingOutline(ParseAssOpen("m 0 0 l 10 0"), 0.0, DrawingStrokeCap::Flat, DrawingStrokeJoin::Bevel, result));
	EXPECT_TRUE(result.commands.empty());

	ASSERT_TRUE(TryDrawingPatternOutline(ParseAssOpen("m 0 0 l 10 0"),
		2.0,
		DrawingStrokeCap::Flat,
		DrawingStrokeJoin::Bevel,
		0.0,
		1.0,
		0.0,
		result));
	EXPECT_TRUE(result.commands.empty());

	ASSERT_TRUE(TryDrawingPatternOutline(ParseAssOpen("m 0 0 l 10 0"),
		2.0,
		DrawingStrokeCap::Flat,
		DrawingStrokeJoin::Bevel,
		1.0,
		-1.0,
		0.0,
		result));
	EXPECT_TRUE(result.commands.empty());

	bool contains = false;
	EXPECT_FALSE(TryDrawingContainsPoint(MakeRect(0.0, 0.0, 10.0, 10.0),
		std::numeric_limits<double>::infinity(), 5.0, contains));
	EXPECT_FALSE(TryDrawingOutline(ParseAssOpen("m 0 0 l 10 0"),
		std::numeric_limits<double>::quiet_NaN(), DrawingStrokeCap::Flat, DrawingStrokeJoin::Bevel, result));
}
