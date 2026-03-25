#include <main.h>

#include "../../src/subtitle_grid_ops.h"

#include <memory>
#include <vector>

namespace {

struct grid_fixture {
	EntryList<AssDialogue> events;
	std::vector<std::unique_ptr<AssDialogue>> storage;

	AssDialogue *AddLine(int row, int start, int end) {
		storage.push_back(std::make_unique<AssDialogue>());
		auto *line = storage.back().get();
		line->Row = row;
		line->Start = start;
		line->End = end;
		events.push_back(*line);
		return line;
	}

	std::vector<AssDialogue *> Ordered() const {
		std::vector<AssDialogue *> ordered;
		for (auto& line : events)
			ordered.push_back(const_cast<AssDialogue *>(&line));
		return ordered;
	}
};

}

TEST(subtitle_grid_ops, create_line_after_uses_previous_end_and_style) {
	AssDialogue current;
	current.End = 2500;
	current.Style = "Alt";

	auto created = aegisub::subtitle_grid_ops::CreateLineAfter(current, 700);

	ASSERT_TRUE(created);
	EXPECT_EQ(2500, static_cast<int>(created->Start));
	EXPECT_EQ(3200, static_cast<int>(created->End));
	EXPECT_EQ(current.Style.get(), created->Style.get());
}

TEST(subtitle_grid_ops, move_selection_up_swaps_with_previous_unselected_line) {
	grid_fixture fixture;
	auto *first = fixture.AddLine(0, 0, 100);
	auto *second = fixture.AddLine(1, 100, 200);
	auto *third = fixture.AddLine(2, 200, 300);

	Selection selection = {second};

	ASSERT_TRUE(aegisub::subtitle_grid_ops::MoveSelectionUp(fixture.events, selection));

	auto ordered = fixture.Ordered();
	ASSERT_EQ(3u, ordered.size());
	EXPECT_EQ(second, ordered[0]);
	EXPECT_EQ(first, ordered[1]);
	EXPECT_EQ(third, ordered[2]);
}

TEST(subtitle_grid_ops, move_selection_down_keeps_selected_block_contiguous) {
	grid_fixture fixture;
	auto *first = fixture.AddLine(0, 0, 100);
	auto *second = fixture.AddLine(1, 100, 200);
	auto *third = fixture.AddLine(2, 200, 300);
	auto *fourth = fixture.AddLine(3, 300, 400);

	Selection selection = {second, third};

	ASSERT_TRUE(aegisub::subtitle_grid_ops::MoveSelectionDown(fixture.events, selection));

	auto ordered = fixture.Ordered();
	ASSERT_EQ(4u, ordered.size());
	EXPECT_EQ(first, ordered[0]);
	EXPECT_EQ(fourth, ordered[1]);
	EXPECT_EQ(second, ordered[2]);
	EXPECT_EQ(third, ordered[3]);
}

TEST(subtitle_grid_ops, swap_selection_requires_exactly_two_lines) {
	grid_fixture fixture;
	auto *first = fixture.AddLine(0, 0, 100);
	auto *second = fixture.AddLine(1, 100, 200);
	auto *third = fixture.AddLine(2, 200, 300);

	EXPECT_FALSE(aegisub::subtitle_grid_ops::SwapSelection({first}));
	EXPECT_TRUE(aegisub::subtitle_grid_ops::SwapSelection({first, third}));

	auto ordered = fixture.Ordered();
	ASSERT_EQ(3u, ordered.size());
	EXPECT_EQ(third, ordered[0]);
	EXPECT_EQ(second, ordered[1]);
	EXPECT_EQ(first, ordered[2]);
}
