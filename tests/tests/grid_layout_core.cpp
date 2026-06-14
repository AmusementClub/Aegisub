#include "../../src/grid_core/grid_layout.h"

#include <gtest/gtest.h>

using aegisub::grid::CalculateGridLayout;
using aegisub::grid::GridLayoutInput;

TEST(grid_layout_core, subtracts_vertical_scrollbar_from_grid_width) {
	auto layout = CalculateGridLayout(GridLayoutInput{
		.client_width = 1166,
		.client_height = 501,
		.vertical_scrollbar_width = 21,
		.line_height = 20,
		.row_count = 100,
		.first_visible_row = 0,
	});

	EXPECT_EQ(1166, layout.client_width);
	EXPECT_EQ(501, layout.client_height);
	EXPECT_EQ(1145, layout.grid_width);
}

TEST(grid_layout_core, includes_one_extra_partially_visible_row) {
	auto layout = CalculateGridLayout(GridLayoutInput{
		.client_width = 400,
		.client_height = 41,
		.vertical_scrollbar_width = 0,
		.line_height = 20,
		.row_count = 100,
		.first_visible_row = 0,
	});

	EXPECT_EQ(3, layout.rows_per_screen);
	EXPECT_EQ(3, layout.rows_to_draw);
}

TEST(grid_layout_core, clamps_rows_to_remaining_dialogue_count) {
	auto layout = CalculateGridLayout(GridLayoutInput{
		.client_width = 400,
		.client_height = 100,
		.vertical_scrollbar_width = 0,
		.line_height = 20,
		.row_count = 3,
		.first_visible_row = 2,
	});

	EXPECT_EQ(6, layout.rows_per_screen);
	EXPECT_EQ(1, layout.rows_to_draw);
}

TEST(grid_layout_core, clamps_degenerate_inputs) {
	auto layout = CalculateGridLayout(GridLayoutInput{
		.client_width = -10,
		.client_height = -20,
		.vertical_scrollbar_width = -5,
		.line_height = 0,
		.row_count = -1,
		.first_visible_row = 7,
	});

	EXPECT_EQ(0, layout.client_width);
	EXPECT_EQ(0, layout.client_height);
	EXPECT_EQ(0, layout.grid_width);
	EXPECT_EQ(1, layout.rows_per_screen);
	EXPECT_EQ(0, layout.rows_to_draw);
}
