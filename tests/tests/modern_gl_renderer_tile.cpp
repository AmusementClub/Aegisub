#include <main.h>

#include "../../src/modern_gl_renderer_tile.h"

TEST(modern_gl_renderer_tile, splits_large_frame_into_expected_tiles) {
	auto layout = BuildModernGLTileLayout(100, 70, 4, 64, true, false);

	ASSERT_EQ(2, layout.texture_rows);
	ASSERT_EQ(2, layout.texture_cols);
	ASSERT_EQ(4u, layout.tiles.size());

	auto const& top_left = layout.tiles[0];
	EXPECT_EQ(0, top_left.source_x);
	EXPECT_EQ(0, top_left.source_y);
	EXPECT_EQ(64, top_left.source_w);
	EXPECT_EQ(64, top_left.source_h);
	EXPECT_EQ(64, top_left.texture_w);
	EXPECT_EQ(64, top_left.texture_h);
	EXPECT_FLOAT_EQ(0.0f, top_left.u1);
	EXPECT_FLOAT_EQ(0.0f, top_left.v1);
	EXPECT_FLOAT_EQ(63.0f / 64.0f, top_left.u2);
	EXPECT_FLOAT_EQ(63.0f / 64.0f, top_left.v2);
	EXPECT_EQ(0, top_left.data_offset);

	auto const& bottom_right = layout.tiles[3];
	EXPECT_EQ(62, bottom_right.source_x);
	EXPECT_EQ(62, bottom_right.source_y);
	EXPECT_EQ(38, bottom_right.source_w);
	EXPECT_EQ(8, bottom_right.source_h);
	EXPECT_EQ(64, bottom_right.texture_w);
	EXPECT_EQ(8, bottom_right.texture_h);
	EXPECT_FLOAT_EQ(63.0f, bottom_right.x1);
	EXPECT_FLOAT_EQ(63.0f, bottom_right.y1);
	EXPECT_FLOAT_EQ(126.0f, bottom_right.x2);
	EXPECT_FLOAT_EQ(70.0f, bottom_right.y2);
	EXPECT_EQ((62 * 100 + 62) * 4, bottom_right.data_offset);
}

TEST(modern_gl_renderer_tile, falls_back_to_square_textures_when_needed) {
	auto layout = BuildModernGLTileLayout(80, 20, 4, 64, false, false);

	ASSERT_EQ(2u, layout.tiles.size());
	EXPECT_EQ(64, layout.tiles[0].texture_w);
	EXPECT_EQ(64, layout.tiles[0].texture_h);
	EXPECT_EQ(32, layout.tiles[1].texture_w);
	EXPECT_EQ(32, layout.tiles[1].texture_h);
}

TEST(modern_gl_renderer_tile, builds_non_flipped_ortho_matrix) {
	auto matrix = BuildModernGLOrthoMatrix(320, 240, false);

	EXPECT_FLOAT_EQ(2.0f / 320.0f, matrix[0]);
	EXPECT_FLOAT_EQ(-2.0f / 240.0f, matrix[5]);
	EXPECT_FLOAT_EQ(-1.0f, matrix[12]);
	EXPECT_FLOAT_EQ(1.0f, matrix[13]);
	EXPECT_FLOAT_EQ(1.0f, matrix[15]);
}

TEST(modern_gl_renderer_tile, builds_flipped_ortho_matrix) {
	auto matrix = BuildModernGLOrthoMatrix(320, 240, true);

	EXPECT_FLOAT_EQ(2.0f / 320.0f, matrix[0]);
	EXPECT_FLOAT_EQ(2.0f / 240.0f, matrix[5]);
	EXPECT_FLOAT_EQ(-1.0f, matrix[12]);
	EXPECT_FLOAT_EQ(-1.0f, matrix[13]);
	EXPECT_FLOAT_EQ(1.0f, matrix[15]);
}
