#pragma once

#include <vector>

namespace aegisub::subtitle_grid_selection_policy {

struct ModifierState {
	bool shift = false;
	bool ctrl = false;
	bool alt = false;
};

struct MouseSelectionInput {
	int row_count = 0;
	int target_row = -1;
	int anchor_row = -1;
	std::vector<int> selected_rows;
	bool click = false;
	bool double_click = false;
	bool dragging = false;
	ModifierState modifiers;
};

struct KeyboardSelectionInput {
	int row_count = 0;
	int active_row = -1;
	int anchor_row = -1;
	std::vector<int> selected_rows;
	int direction = 0;
	int step = 0;
	ModifierState modifiers;
	// Grid navigation resolves visible destinations back to document rows.
	int target_row = -1;
};

struct RowInsertSelectionInput {
	int row_count = 0;
	int inserted_row = -1;
};

struct RowsDeleteSelectionInput {
	int row_count_before = 0;
	int first_deleted_row = -1;
	int deleted_row_count = 0;
};

struct SelectionPlan {
	bool handled = false;
	bool set_active = false;
	bool set_selection = false;
	bool activate_media = false;
	bool make_active_visible = false;
	int active_row = -1;
	int anchor_row = -1;
	std::vector<int> selected_rows;
};

SelectionPlan PlanMouseSelection(MouseSelectionInput input);
SelectionPlan PlanKeyboardSelection(KeyboardSelectionInput input);
SelectionPlan PlanRowInsertSelection(RowInsertSelectionInput input);
SelectionPlan PlanRowsDeleteSelection(RowsDeleteSelectionInput input);

}
