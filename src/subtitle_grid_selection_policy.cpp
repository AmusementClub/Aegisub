#include "subtitle_grid_selection_policy.h"

#include <algorithm>

namespace aegisub::subtitle_grid_selection_policy {
namespace {

bool is_valid_row(int row_count, int row) {
	return row >= 0 && row < row_count;
}

int clamp_row(int row_count, int row) {
	if (row_count <= 0)
		return -1;
	return std::clamp(row, 0, row_count - 1);
}

std::vector<int> normalized_rows(std::vector<int> rows, int row_count) {
	rows.erase(std::remove_if(rows.begin(), rows.end(), [=](int row) {
		return !is_valid_row(row_count, row);
	}), rows.end());
	std::sort(rows.begin(), rows.end());
	rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
	return rows;
}

bool contains_row(std::vector<int> const& rows, int row) {
	return std::binary_search(rows.begin(), rows.end(), row);
}

void insert_row(std::vector<int>& rows, int row) {
	auto it = std::lower_bound(rows.begin(), rows.end(), row);
	if (it == rows.end() || *it != row)
		rows.insert(it, row);
}

void erase_row(std::vector<int>& rows, int row) {
	auto it = std::lower_bound(rows.begin(), rows.end(), row);
	if (it != rows.end() && *it == row)
		rows.erase(it);
}

void add_range(std::vector<int>& rows, int first, int last) {
	if (first > last)
		std::swap(first, last);

	// Both inputs are sorted and unique. Merge the requested interval in one
	// pass; inserting each row with lower_bound turns a long drag into a
	// quadratic operation as the selected range grows.
	auto const range_size = static_cast<size_t>(last) - static_cast<size_t>(first) + 1u;
	if (rows.empty()) {
		rows.reserve(range_size);
		for (int row = first;; ++row) {
			rows.push_back(row);
			if (row == last)
				break;
		}
		return;
	}

	std::vector<int> merged;
	merged.reserve(rows.size() + range_size);
	auto existing = rows.begin();
	for (int row = first;; ++row) {
		while (existing != rows.end() && *existing < row)
			merged.push_back(*existing++);
		if (existing == rows.end() || *existing > row)
			merged.push_back(row);
		else {
			merged.push_back(row);
			++existing;
		}
		if (row == last)
			break;
	}
	merged.insert(merged.end(), existing, rows.end());
	rows.swap(merged);
}

int normalized_anchor(int row_count, int anchor_row, int fallback_row) {
	return is_valid_row(row_count, anchor_row) ? anchor_row : fallback_row;
}

SelectionPlan active_plan(int row) {
	SelectionPlan plan;
	plan.handled = true;
	plan.set_active = true;
	plan.active_row = row;
	plan.anchor_row = row;
	return plan;
}

}

SelectionPlan PlanMouseSelection(MouseSelectionInput input) {
	if (!is_valid_row(input.row_count, input.target_row))
		return {};
	if (!input.click && !input.double_click && !input.dragging)
		return {};

	auto const modifiers = input.modifiers;
	auto plan = active_plan(input.target_row);

	if (input.click && modifiers.ctrl && !modifiers.shift && !modifiers.alt) {
		auto selected = normalized_rows(std::move(input.selected_rows), input.row_count);
		bool const target_selected = contains_row(selected, input.target_row);
		if (target_selected && selected.size() == 1)
			return plan;

		if (target_selected)
			erase_row(selected, input.target_row);
		else
			insert_row(selected, input.target_row);
		plan.set_selection = true;
		plan.selected_rows = std::move(selected);
		return plan;
	}

	if ((input.click || input.double_click) && !modifiers.shift && !modifiers.ctrl && !modifiers.alt) {
		plan.set_selection = true;
		plan.activate_media = input.double_click;
		plan.selected_rows = {input.target_row};
		return plan;
	}

	if (input.click && !modifiers.shift && !modifiers.ctrl && modifiers.alt)
		return plan;

	if ((input.click && modifiers.shift && !modifiers.alt) || input.dragging) {
		int const anchor = normalized_anchor(input.row_count, input.anchor_row, input.target_row);
		std::vector<int> next_selection;
		if (modifiers.ctrl)
			next_selection = normalized_rows(std::move(input.selected_rows), input.row_count);
		add_range(next_selection, input.target_row, anchor);
		plan.anchor_row = anchor;
		plan.set_selection = true;
		plan.selected_rows = std::move(next_selection);
		return plan;
	}

	return plan;
}

SelectionPlan PlanKeyboardSelection(KeyboardSelectionInput input) {
	if (input.row_count <= 0)
		return {};
	bool const explicit_target = input.target_row != -1;
	if (explicit_target ? !is_valid_row(input.row_count, input.target_row) : input.direction == 0 || input.step <= 0)
		return {};

	auto const active = is_valid_row(input.row_count, input.active_row) ? input.active_row : 0;
	auto const next = explicit_target ? input.target_row : clamp_row(input.row_count, active + input.direction * input.step);
	auto const modifiers = input.modifiers;
	auto plan = active_plan(next);

	if (!modifiers.ctrl && !modifiers.shift && !modifiers.alt) {
		plan.set_selection = true;
		plan.selected_rows = {next};
		return plan;
	}

	if (modifiers.alt && !modifiers.shift && !modifiers.ctrl)
		return plan;

	if (modifiers.shift && !modifiers.alt) {
		int const anchor = normalized_anchor(input.row_count, input.anchor_row, active);
		std::vector<int> next_selection;
		if (modifiers.ctrl)
			next_selection = normalized_rows(std::move(input.selected_rows), input.row_count);
		add_range(next_selection, next, anchor);
		plan.anchor_row = anchor;
		plan.set_selection = true;
		plan.make_active_visible = true;
		plan.selected_rows = std::move(next_selection);
		return plan;
	}

	return plan;
}

SelectionPlan PlanRowInsertSelection(RowInsertSelectionInput input) {
	if (!is_valid_row(input.row_count, input.inserted_row))
		return {};

	auto plan = active_plan(input.inserted_row);
	plan.set_selection = true;
	plan.make_active_visible = true;
	plan.selected_rows = {input.inserted_row};
	return plan;
}

SelectionPlan PlanRowsDeleteSelection(RowsDeleteSelectionInput input) {
	if (input.row_count_before <= 0
		|| input.first_deleted_row < 0
		|| input.deleted_row_count <= 0
		|| input.first_deleted_row > input.row_count_before
		|| input.deleted_row_count > input.row_count_before - input.first_deleted_row)
		return {};

	auto const row_count_after = input.row_count_before - input.deleted_row_count;
	SelectionPlan plan;
	plan.handled = true;
	plan.set_active = true;
	plan.set_selection = true;
	plan.make_active_visible = row_count_after > 0;
	if (row_count_after <= 0) {
		plan.active_row = -1;
		plan.anchor_row = -1;
		return plan;
	}

	auto const active = clamp_row(row_count_after, input.first_deleted_row);
	plan.active_row = active;
	plan.anchor_row = active;
	plan.selected_rows = {active};
	return plan;
}

}
