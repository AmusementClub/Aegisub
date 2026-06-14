#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/presentation/presentation_contract.h"
#include "../../src/presentation/subtitle_grid_projection.h"

#include <vector>

using namespace aegisub::presentation;

TEST(presentation_contract, command_flags_are_framework_neutral_bitmasks) {
	std::uint32_t flags = CommandFlag::Validate | CommandFlag::Toggle;
	flags = flags | CommandFlag::DynamicName;

	EXPECT_TRUE(HasFlag(flags, CommandFlag::Validate));
	EXPECT_TRUE(HasFlag(flags, CommandFlag::Toggle));
	EXPECT_TRUE(HasFlag(flags, CommandFlag::DynamicName));
	EXPECT_FALSE(HasFlag(flags, CommandFlag::Radio));
}

TEST(presentation_contract, projects_dialogue_line_to_framework_neutral_grid_row) {
	AssDialogue line;
	line.Id = 42;
	line.Row = 7;
	line.Comment = true;
	line.Layer = 3;
	line.Start = 1200;
	line.End = 3450;
	line.Margin = {{11, 22, 33}};
	line.Style = "Alt";
	line.Actor = "Actor";
	line.Effect = "fx";
	line.Text = "{\\i1}Hello";

	SubtitleGridRowState state;
	state.selected = true;
	state.active = true;
	state.visible_at_current_frame = true;

	auto row = ProjectSubtitleGridRow(line, 5, state);

	EXPECT_EQ(42, row.line_id);
	EXPECT_EQ(5, row.row_index);
	EXPECT_TRUE(row.comment);
	EXPECT_EQ(3, row.layer);
	EXPECT_EQ(1200, row.start_ms);
	EXPECT_EQ(3450, row.end_ms);
	EXPECT_EQ((std::array<int, 3>{{11, 22, 33}}), row.margins);
	EXPECT_EQ("Alt", row.style);
	EXPECT_EQ("Actor", row.actor);
	EXPECT_EQ("fx", row.effect);
	EXPECT_EQ("{\\i1}Hello", row.text);
	EXPECT_TRUE(row.state.selected);
	EXPECT_TRUE(row.state.active);
	EXPECT_TRUE(row.state.visible_at_current_frame);
}

TEST(presentation_contract, builds_paged_subtitle_grid_window_with_revision) {
	AssDialogue first;
	first.Id = 1;
	first.Row = 0;
	first.Text = "first";
	AssDialogue second;
	second.Id = 2;
	second.Row = 1;
	second.Text = "second";
	AssDialogue third;
	third.Id = 3;
	third.Row = 2;
	third.Text = "third";

	std::vector<AssDialogue const*> rows{&first, &second, &third};
	VisibleSubtitleRowsRequest request;
	request.first_row = 1;
	request.row_count = 5;
	request.known_revision = 10;

	auto window = BuildSubtitleGridWindow(rows, request, 11, [](AssDialogue const& line) {
		SubtitleGridRowState state;
		state.selected = line.Id == 2;
		return state;
	});

	ASSERT_EQ(2u, window.rows.size());
	EXPECT_EQ(11u, window.revision);
	EXPECT_EQ(1, window.first_row);
	EXPECT_EQ(3, window.total_rows);
	EXPECT_EQ(2, window.rows[0].line_id);
	EXPECT_EQ("second", window.rows[0].text);
	EXPECT_TRUE(window.rows[0].state.selected);
	EXPECT_EQ(3, window.rows[1].line_id);
	EXPECT_EQ("third", window.rows[1].text);
	EXPECT_FALSE(window.rows[1].state.selected);
}
