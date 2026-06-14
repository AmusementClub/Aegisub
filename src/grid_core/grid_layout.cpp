// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "grid_layout.h"

#include <algorithm>

namespace aegisub::grid {

GridLayout CalculateGridLayout(GridLayoutInput const& input) {
	GridLayout layout;
	layout.client_width = std::max(0, input.client_width);
	layout.client_height = std::max(0, input.client_height);

	int const scrollbar_width = std::max(0, input.vertical_scrollbar_width);
	layout.grid_width = std::max(0, layout.client_width - scrollbar_width);

	int const line_height = std::max(1, input.line_height);
	layout.rows_per_screen = layout.client_height / line_height + 1;

	int const row_count = std::max(0, input.row_count);
	int const first_visible_row = std::clamp(input.first_visible_row, 0, row_count);
	int const remaining_rows = row_count - first_visible_row;
	layout.rows_to_draw = std::clamp(layout.rows_per_screen, 0, remaining_rows);

	return layout;
}

} // namespace aegisub::grid
