#include <main.h>

#include "../../src/subtitle_grid_selection_policy.h"

#include <vector>

namespace policy = aegisub::subtitle_grid_selection_policy;

TEST(subtitle_grid_selection_policy, plain_mouse_click_replaces_selection_and_updates_anchor) {
	auto plan = policy::PlanMouseSelection({
		5,
		2,
		0,
		{0, 1},
		true,
		false,
		false,
		{},
	});

	ASSERT_TRUE(plan.handled);
	EXPECT_TRUE(plan.set_active);
	EXPECT_TRUE(plan.set_selection);
	EXPECT_EQ(2, plan.active_row);
	EXPECT_EQ(2, plan.anchor_row);
	EXPECT_EQ(std::vector<int>{2}, plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, double_click_requests_media_activation) {
	auto plan = policy::PlanMouseSelection({
		5,
		3,
		1,
		{1},
		false,
		true,
		false,
		{},
	});

	ASSERT_TRUE(plan.handled);
	EXPECT_TRUE(plan.activate_media);
	EXPECT_EQ(std::vector<int>{3}, plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, ctrl_click_toggles_without_clearing_last_selected_row) {
	auto deselect_last = policy::PlanMouseSelection({
		5,
		2,
		2,
		{2},
		true,
		false,
		false,
		{false, true, false},
	});
	ASSERT_TRUE(deselect_last.handled);
	EXPECT_TRUE(deselect_last.set_active);
	EXPECT_FALSE(deselect_last.set_selection);

	auto add = policy::PlanMouseSelection({
		5,
		3,
		2,
		{1, 2},
		true,
		false,
		false,
		{false, true, false},
	});
	ASSERT_TRUE(add.set_selection);
	EXPECT_EQ((std::vector<int>{1, 2, 3}), add.selected_rows);

	auto remove = policy::PlanMouseSelection({
		5,
		2,
		2,
		{1, 2, 3},
		true,
		false,
		false,
		{false, true, false},
	});
	ASSERT_TRUE(remove.set_selection);
	EXPECT_EQ((std::vector<int>{1, 3}), remove.selected_rows);
}

TEST(subtitle_grid_selection_policy, shift_click_selects_anchor_range_and_ctrl_extends) {
	auto replace = policy::PlanMouseSelection({
		8,
		5,
		2,
		{0, 7},
		true,
		false,
		false,
		{true, false, false},
	});
	ASSERT_TRUE(replace.set_selection);
	EXPECT_EQ(2, replace.anchor_row);
	EXPECT_EQ((std::vector<int>{2, 3, 4, 5}), replace.selected_rows);

	auto extend = policy::PlanMouseSelection({
		8,
		5,
		2,
		{0, 7},
		true,
		false,
		false,
		{true, true, false},
	});
	ASSERT_TRUE(extend.set_selection);
	EXPECT_EQ((std::vector<int>{0, 2, 3, 4, 5, 7}), extend.selected_rows);
}

TEST(subtitle_grid_selection_policy, dragging_uses_anchor_even_without_shift) {
	auto plan = policy::PlanMouseSelection({
		6,
		4,
		1,
		{},
		false,
		false,
		true,
		{},
	});

	ASSERT_TRUE(plan.handled);
	ASSERT_TRUE(plan.set_selection);
	EXPECT_EQ(1, plan.anchor_row);
	EXPECT_EQ((std::vector<int>{1, 2, 3, 4}), plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, ctrl_drag_keeps_selected_rows_inside_added_range) {
	auto plan = policy::PlanMouseSelection({
		8,
		5,
		2,
		{0, 2, 4, 7},
		false,
		false,
		true,
		{false, true, false},
	});

	ASSERT_TRUE(plan.handled);
	ASSERT_TRUE(plan.set_selection);
	EXPECT_EQ(2, plan.anchor_row);
	EXPECT_EQ((std::vector<int>{0, 2, 3, 4, 5, 7}), plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, ctrl_drag_foldback_keeps_rows_already_crossed) {
	auto outward = policy::PlanMouseSelection({
		8,
		6,
		2,
		{0, 7},
		false,
		false,
		true,
		{false, true, false},
	});
	ASSERT_TRUE(outward.set_selection);

	auto folded_back = policy::PlanMouseSelection({
		8,
		4,
		2,
		std::move(outward.selected_rows),
		false,
		false,
		true,
		{false, true, false},
	});

	ASSERT_TRUE(folded_back.set_selection);
	EXPECT_EQ((std::vector<int>{0, 2, 3, 4, 5, 6, 7}), folded_back.selected_rows);
}

TEST(subtitle_grid_selection_policy, keyboard_plain_move_replaces_selection) {
	auto plan = policy::PlanKeyboardSelection({
		10,
		4,
		4,
		{4},
		1,
		1,
		{},
	});

	ASSERT_TRUE(plan.handled);
	EXPECT_EQ(5, plan.active_row);
	EXPECT_EQ(5, plan.anchor_row);
	ASSERT_TRUE(plan.set_selection);
	EXPECT_EQ(std::vector<int>{5}, plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, keyboard_shift_move_selects_anchor_range) {
	auto plan = policy::PlanKeyboardSelection({
		10,
		4,
		2,
		{4},
		1,
		3,
		{true, false, false},
	});

	ASSERT_TRUE(plan.handled);
	EXPECT_EQ(7, plan.active_row);
	EXPECT_EQ(2, plan.anchor_row);
	EXPECT_TRUE(plan.make_active_visible);
	ASSERT_TRUE(plan.set_selection);
	EXPECT_EQ((std::vector<int>{2, 3, 4, 5, 6, 7}), plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, keyboard_alt_move_changes_active_only) {
	auto plan = policy::PlanKeyboardSelection({
		5,
		2,
		2,
		{2},
		-1,
		1,
		{false, false, true},
	});

	ASSERT_TRUE(plan.handled);
	EXPECT_TRUE(plan.set_active);
	EXPECT_FALSE(plan.set_selection);
	EXPECT_EQ(1, plan.active_row);
}

TEST(subtitle_grid_selection_policy, explicit_keyboard_destination_skips_hidden_rows) {
	policy::KeyboardSelectionInput input;
	input.row_count = 12;
	input.active_row = 2;
	input.anchor_row = 2;
	input.selected_rows = {2};
	input.direction = 1;
	input.step = 1;
	input.target_row = 8;

	auto plan = policy::PlanKeyboardSelection(input);

	ASSERT_TRUE(plan.handled);
	EXPECT_EQ(8, plan.active_row);
	EXPECT_EQ(8, plan.anchor_row);
	EXPECT_EQ(std::vector<int>{8}, plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, explicit_keyboard_shift_destination_includes_hidden_range) {
	policy::KeyboardSelectionInput input;
	input.row_count = 12;
	input.active_row = 2;
	input.anchor_row = 1;
	input.selected_rows = {1, 2};
	input.target_row = 8;
	input.modifiers.shift = true;

	auto plan = policy::PlanKeyboardSelection(input);

	ASSERT_TRUE(plan.set_selection);
	EXPECT_EQ(8, plan.active_row);
	EXPECT_EQ(1, plan.anchor_row);
	EXPECT_TRUE(plan.make_active_visible);
	EXPECT_EQ((std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8}), plan.selected_rows);
	input.active_row = 8;
	input.anchor_row = 9;
	input.target_row = 2;
	plan = policy::PlanKeyboardSelection(input);
	EXPECT_EQ((std::vector<int>{2, 3, 4, 5, 6, 7, 8, 9}), plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, keyboard_ctrl_shift_merges_range_with_existing_selection) {
	policy::KeyboardSelectionInput input;
	input.row_count = 12;
	input.active_row = 2;
	input.anchor_row = 2;
	input.selected_rows = {11, 4, 0, 4, -1, 12};
	input.target_row = 8;
	input.modifiers = {.shift = true, .ctrl = true};

	auto plan = policy::PlanKeyboardSelection(input);

	ASSERT_TRUE(plan.set_selection);
	EXPECT_EQ(8, plan.active_row);
	EXPECT_EQ(2, plan.anchor_row);
	EXPECT_EQ((std::vector<int>{0, 2, 3, 4, 5, 6, 7, 8, 11}), plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, explicit_keyboard_destination_handles_home_end_and_rejects_invalid_rows) {
	policy::KeyboardSelectionInput input;
	input.row_count = 7;
	input.active_row = 3;
	input.target_row = 0;

	auto home = policy::PlanKeyboardSelection(input);
	ASSERT_TRUE(home.handled);
	EXPECT_EQ(std::vector<int>{0}, home.selected_rows);
	input.target_row = 6;
	auto end = policy::PlanKeyboardSelection(input);
	ASSERT_TRUE(end.handled);
	EXPECT_EQ(std::vector<int>{6}, end.selected_rows);
	input.target_row = 7;
	EXPECT_FALSE(policy::PlanKeyboardSelection(input).handled);
	input.target_row = -2;
	EXPECT_FALSE(policy::PlanKeyboardSelection(input).handled);
}

TEST(subtitle_grid_selection_policy, invalid_rows_are_ignored_and_targets_are_clamped) {
	auto mouse = policy::PlanMouseSelection({
		3,
		1,
		-1,
		{-1, 2, 4},
		true,
		false,
		false,
		{true, true, false},
	});
	ASSERT_TRUE(mouse.set_selection);
	EXPECT_EQ((std::vector<int>{1, 2}), mouse.selected_rows);

	auto keyboard = policy::PlanKeyboardSelection({
		3,
		2,
		2,
		{2},
		1,
		99,
		{},
	});
	ASSERT_TRUE(keyboard.handled);
	EXPECT_EQ(2, keyboard.active_row);
	EXPECT_EQ(std::vector<int>{2}, keyboard.selected_rows);
}

TEST(subtitle_grid_selection_policy, row_insert_selects_inserted_row) {
	auto plan = policy::PlanRowInsertSelection({4, 2});

	ASSERT_TRUE(plan.handled);
	EXPECT_TRUE(plan.set_active);
	EXPECT_TRUE(plan.set_selection);
	EXPECT_TRUE(plan.make_active_visible);
	EXPECT_EQ(2, plan.active_row);
	EXPECT_EQ(2, plan.anchor_row);
	EXPECT_EQ(std::vector<int>{2}, plan.selected_rows);
}

TEST(subtitle_grid_selection_policy, rows_delete_selects_nearest_surviving_row) {
	auto middle = policy::PlanRowsDeleteSelection({5, 2, 2});
	ASSERT_TRUE(middle.handled);
	EXPECT_EQ(2, middle.active_row);
	EXPECT_EQ(std::vector<int>{2}, middle.selected_rows);

	auto tail = policy::PlanRowsDeleteSelection({5, 3, 2});
	ASSERT_TRUE(tail.handled);
	EXPECT_EQ(2, tail.active_row);
	EXPECT_EQ(std::vector<int>{2}, tail.selected_rows);
}

TEST(subtitle_grid_selection_policy, rows_delete_can_clear_empty_grid_selection) {
	auto plan = policy::PlanRowsDeleteSelection({2, 0, 2});

	ASSERT_TRUE(plan.handled);
	EXPECT_TRUE(plan.set_active);
	EXPECT_TRUE(plan.set_selection);
	EXPECT_FALSE(plan.make_active_visible);
	EXPECT_EQ(-1, plan.active_row);
	EXPECT_EQ(-1, plan.anchor_row);
	EXPECT_TRUE(plan.selected_rows.empty());
}

TEST(subtitle_grid_selection_policy, row_structure_invalid_inputs_are_unhandled) {
	EXPECT_FALSE(policy::PlanRowInsertSelection({3, 3}).handled);
	EXPECT_FALSE(policy::PlanRowsDeleteSelection({3, 3, 1}).handled);
	EXPECT_FALSE(policy::PlanRowsDeleteSelection({3, 1, 0}).handled);
	EXPECT_FALSE(policy::PlanRowsDeleteSelection({3, 2, 2}).handled);
}
