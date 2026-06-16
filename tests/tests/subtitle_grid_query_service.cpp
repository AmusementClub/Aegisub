#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/presentation/subtitle_grid_query_service.h"

#include <array>

namespace {

AssDialogue *AppendDialogue(
	AssFile& file,
	int row,
	int id,
	bool comment,
	int layer,
	int start_ms,
	int end_ms,
	char const* style,
	char const* actor,
	char const* effect,
	char const* text) {
	auto* line = new AssDialogue;
	line->Id = id;
	line->Row = row;
	line->Comment = comment;
	line->Layer = layer;
	line->Start = start_ms;
	line->End = end_ms;
	line->Margin = {{row + 10, row + 20, row + 30}};
	line->Style = style;
	line->Actor = actor;
	line->Effect = effect;
	line->Text = text;
	file.Events.push_back(*line);
	return line;
}

}

using namespace aegisub::presentation;

TEST(subtitle_grid_query_service, queries_visible_rows_from_ass_file_with_selection_state) {
	AssFile file;

	AppendDialogue(file, 0, 100, false, 0, 0, 1000, "Default", "", "", "zero");
	auto const* selected = AppendDialogue(file, 1, 101, true, 2, 1000, 2000, "Alt", "Actor", "fx", "{\\i1}one");
	auto const* active = AppendDialogue(file, 2, 102, false, 4, 2000, 3500, "Default", "Lead", "", "two");
	AppendDialogue(file, 3, 103, false, 0, 3500, 5000, "Default", "", "", "three");

	VisibleSubtitleRowsRequest request;
	request.first_row = 1;
	request.row_count = 2;
	request.known_revision = 6;

	auto window = QueryVisibleSubtitleRows(file, request, 7, [&](AssDialogue const& line) {
		SubtitleGridRowState state;
		state.selected = &line == selected || &line == active;
		state.active = &line == active;
		return state;
	});

	ASSERT_EQ(2u, window.rows.size());
	EXPECT_EQ(7u, window.revision);
	EXPECT_EQ(1, window.first_row);
	EXPECT_EQ(4, window.total_rows);

	EXPECT_EQ(101, window.rows[0].line_id);
	EXPECT_EQ(1, window.rows[0].row_index);
	EXPECT_TRUE(window.rows[0].comment);
	EXPECT_EQ(2, window.rows[0].layer);
	EXPECT_EQ(1000, window.rows[0].start_ms);
	EXPECT_EQ(2000, window.rows[0].end_ms);
	EXPECT_EQ((std::array<int, 3>{{11, 21, 31}}), window.rows[0].margins);
	EXPECT_EQ("Alt", window.rows[0].style);
	EXPECT_EQ("Actor", window.rows[0].actor);
	EXPECT_EQ("fx", window.rows[0].effect);
	EXPECT_EQ("{\\i1}one", window.rows[0].text);
	EXPECT_TRUE(window.rows[0].state.selected);
	EXPECT_FALSE(window.rows[0].state.active);
	EXPECT_FALSE(window.rows[0].state.visible_at_current_frame);

	EXPECT_EQ(102, window.rows[1].line_id);
	EXPECT_EQ(2, window.rows[1].row_index);
	EXPECT_TRUE(window.rows[1].state.selected);
	EXPECT_TRUE(window.rows[1].state.active);
	EXPECT_EQ("two", window.rows[1].text);
}

TEST(subtitle_grid_query_service, const_file_clamps_window_edges) {
	AssFile file;

	AppendDialogue(file, 0, 200, false, 0, 0, 1000, "Default", "", "", "first");
	AppendDialogue(file, 1, 201, false, 0, 1000, 2000, "Default", "", "", "second");

	VisibleSubtitleRowsRequest request;
	request.first_row = -5;
	request.row_count = 1;

	AssFile const& const_file = file;
	auto window = QueryVisibleSubtitleRows(const_file, request, 8);

	ASSERT_EQ(1u, window.rows.size());
	EXPECT_EQ(0, window.first_row);
	EXPECT_EQ(2, window.total_rows);
	EXPECT_EQ(200, window.rows[0].line_id);

	request.first_row = 50;
	request.row_count = 10;
	window = QueryVisibleSubtitleRows(const_file, request, 9);

	EXPECT_TRUE(window.rows.empty());
	EXPECT_EQ(2, window.first_row);
	EXPECT_EQ(2, window.total_rows);
	EXPECT_EQ(9u, window.revision);
}

TEST(subtitle_grid_query_service, honors_requested_column_ids) {
	AssFile file;

	AppendDialogue(file, 0, 300, true, 5, 100, 800, "Alt", "Actor", "fx", "visible text");

	VisibleSubtitleRowsRequest request;
	request.first_row = 0;
	request.row_count = 1;
	request.column_ids = {SubtitleGridColumnIdActor};

	auto window = QueryVisibleSubtitleRows(file, request, 10);

	ASSERT_EQ(1u, window.rows.size());
	EXPECT_EQ(300, window.rows[0].line_id);
	EXPECT_EQ(0, window.rows[0].row_index);
	EXPECT_TRUE(window.rows[0].comment);
	EXPECT_EQ("Actor", window.rows[0].actor);
	EXPECT_EQ(0, window.rows[0].layer);
	EXPECT_EQ(0, window.rows[0].start_ms);
	EXPECT_EQ(0, window.rows[0].end_ms);
	EXPECT_TRUE(window.rows[0].style.empty());
	EXPECT_TRUE(window.rows[0].effect.empty());
	EXPECT_TRUE(window.rows[0].text.empty());
}
