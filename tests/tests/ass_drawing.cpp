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
