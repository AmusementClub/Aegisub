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
	ExpectPoint(tangent, 6.0, 8.0);
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

	EXPECT_EQ("m 0 0 b 6.672 0 10 3.328 10 10", SerializeAss(path));
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

	ASSERT_TRUE(TryDrawingBoolean(lhs, rhs, DrawingBooleanOp::Xor, result));
	ASSERT_TRUE(TryDrawingContainsPoint(result, 2.0, 2.0, contains));
	EXPECT_TRUE(contains);
	ASSERT_TRUE(TryDrawingContainsPoint(result, 7.0, 7.0, contains));
	EXPECT_FALSE(contains);
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
}
