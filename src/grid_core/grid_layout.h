// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

namespace aegisub::grid {

struct GridLayoutInput {
	int client_width = 0;
	int client_height = 0;
	int vertical_scrollbar_width = 0;
	int line_height = 1;
	int row_count = 0;
	int first_visible_row = 0;
};

struct GridLayout {
	int client_width = 0;
	int client_height = 0;
	int grid_width = 0;
	int rows_per_screen = 0;
	int rows_to_draw = 0;
};

GridLayout CalculateGridLayout(GridLayoutInput const& input);

} // namespace aegisub::grid
