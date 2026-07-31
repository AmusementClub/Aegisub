#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/presentation/presentation_contract.h"
#include "../../src/presentation/subtitle_grid_display.h"
#include "../../src/presentation/subtitle_grid_diff.h"
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
	state.collides_with_active = true;

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
	EXPECT_TRUE(row.state.collides_with_active);
}

TEST(presentation_contract, projects_only_requested_subtitle_grid_columns) {
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

	auto row = ProjectSubtitleGridRow(
		line,
		5,
		state,
		std::vector<std::string>{
			SubtitleGridColumnIdStyle,
			SubtitleGridColumnIdText});

	EXPECT_EQ(42, row.line_id);
	EXPECT_EQ(5, row.row_index);
	EXPECT_TRUE(row.comment);
	EXPECT_TRUE(row.state.selected);
	EXPECT_EQ("Alt", row.style);
	EXPECT_EQ("{\\i1}Hello", row.text);
	EXPECT_EQ(0, row.layer);
	EXPECT_EQ(0, row.start_ms);
	EXPECT_EQ(0, row.end_ms);
	EXPECT_EQ((std::array<int, 3>{{0, 0, 0}}), row.margins);
	EXPECT_TRUE(row.actor.empty());
	EXPECT_TRUE(row.effect.empty());
}

TEST(presentation_contract, cps_projection_includes_timing_and_text_dependencies) {
	AssDialogue line;
	line.Id = 9;
	line.Row = 1;
	line.Layer = 8;
	line.Start = 100;
	line.End = 900;
	line.Style = "Alt";
	line.Text = "caption";

	auto row = ProjectSubtitleGridRow(
		line,
		1,
		{},
		std::vector<std::string>{SubtitleGridColumnIdCps});

	EXPECT_EQ(100, row.start_ms);
	EXPECT_EQ(900, row.end_ms);
	EXPECT_EQ("caption", row.text);
	EXPECT_EQ(0, row.layer);
	EXPECT_TRUE(row.style.empty());
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

TEST(presentation_contract, window_skips_null_rows_without_losing_source_row_index) {
	AssDialogue first;
	first.Id = 1;
	first.Row = 0;
	first.Text = "first";
	AssDialogue third;
	third.Id = 3;
	third.Row = 2;
	third.Text = "third";

	std::vector<AssDialogue const*> rows{&first, nullptr, &third};
	VisibleSubtitleRowsRequest request;
	request.first_row = 0;
	request.row_count = 3;

	auto window = BuildSubtitleGridWindow(rows, request, 15);

	ASSERT_EQ(2u, window.rows.size());
	EXPECT_EQ(0, window.rows[0].row_index);
	EXPECT_EQ(2, window.rows[1].row_index);
	EXPECT_EQ("third", window.rows[1].text);
}

TEST(presentation_contract, formats_grid_cells_without_gui_framework_types) {
	SubtitleGridRow row;
	row.row_index = 4;
	row.layer = 2;
	row.start_ms = 1234;
	row.end_ms = 5678;
	row.margins = {{0, 12, 34}};
	row.style = "Default";
	row.actor = "Actor";
	row.effect = "fx";
	row.text = "{\\i1}Hello{\\i0} world";

	SubtitleGridDisplayOptions options;
	options.time_display_mode = SubtitleTimeDisplayMode::Ass;
	options.override_mode = SubtitleGridOverrideMode::Replace;
	options.override_replacement = "#";

	EXPECT_EQ("5", FormatSubtitleGridCell(row, SubtitleGridColumnIdLineNumber, options));
	EXPECT_EQ("2", FormatSubtitleGridCell(row, SubtitleGridColumnIdLayer, options));
	EXPECT_EQ("0:00:01.23", FormatSubtitleGridCell(row, SubtitleGridColumnIdStart, options));
	EXPECT_EQ("0:00:05.68", FormatSubtitleGridCell(row, SubtitleGridColumnIdEnd, options));
	EXPECT_EQ("", FormatSubtitleGridCell(row, SubtitleGridColumnIdMarginLeft, options));
	EXPECT_EQ("12", FormatSubtitleGridCell(row, SubtitleGridColumnIdMarginRight, options));
	EXPECT_EQ("34", FormatSubtitleGridCell(row, SubtitleGridColumnIdMarginVertical, options));
	EXPECT_EQ("Default", FormatSubtitleGridCell(row, SubtitleGridColumnIdStyle, options));
	EXPECT_EQ("Actor", FormatSubtitleGridCell(row, SubtitleGridColumnIdActor, options));
	EXPECT_EQ("fx", FormatSubtitleGridCell(row, SubtitleGridColumnIdEffect, options));
	EXPECT_EQ("#Hello# world", FormatSubtitleGridCell(row, SubtitleGridColumnIdText, options));

	row.margins = {{-15, 0, -10000}};
	EXPECT_EQ("-15", FormatSubtitleGridCell(row, SubtitleGridColumnIdMarginLeft, options));
	EXPECT_EQ("", FormatSubtitleGridCell(row, SubtitleGridColumnIdMarginRight, options));
	EXPECT_EQ("-10000", FormatSubtitleGridCell(row, SubtitleGridColumnIdMarginVertical, options));
}

TEST(presentation_contract, grid_text_formatter_preserves_override_modes) {
	auto const text = std::string("A{\\b1}B{\\b0}C");

	EXPECT_EQ(text, FormatSubtitleGridText(text, SubtitleGridOverrideMode::Show, "*"));
	EXPECT_EQ("A*B*C", FormatSubtitleGridText(text, SubtitleGridOverrideMode::Replace, "*"));
	EXPECT_EQ("ABC", FormatSubtitleGridText(text, SubtitleGridOverrideMode::Hide, "*"));
}

TEST(presentation_contract, grid_cps_formatter_matches_grid_threshold_text) {
	SubtitleGridRow row;
	row.start_ms = 0;
	row.end_ms = 1000;
	row.text = "abcd";

	SubtitleGridDisplayOptions integer_options;
	EXPECT_EQ(4.0, CalculateSubtitleGridCps(row, integer_options));
	EXPECT_EQ("4", FormatSubtitleGridCell(row, SubtitleGridColumnIdCps, integer_options));

	SubtitleGridDisplayOptions decimal_options;
	decimal_options.show_decimal_cps = true;
	EXPECT_EQ(4.0, CalculateSubtitleGridCps(row, decimal_options));
	EXPECT_EQ("4.0", FormatSubtitleGridCell(row, SubtitleGridColumnIdCps, decimal_options));

	row.text.assign(200, 'x');
	EXPECT_TRUE(FormatSubtitleGridCell(row, SubtitleGridColumnIdCps, integer_options).empty());
}

TEST(presentation_contract, builds_reset_diff_for_grid_shape_commits) {
	auto diff = BuildSubtitleGridDiffFromCommit(
		AssFile::COMMIT_ORDER,
		3,
		4);

	EXPECT_EQ(3u, diff.before_revision);
	EXPECT_EQ(4u, diff.after_revision);
	EXPECT_EQ(SubtitleGridDiffKind::Reset, diff.kind);
	EXPECT_TRUE(diff.requires_full_refresh);
	EXPECT_TRUE(diff.upserted_rows.empty());
}

TEST(presentation_contract, builds_single_row_diff_for_text_commit) {
	AssDialogue line;
	line.Id = 55;
	line.Row = 9;
	line.Text = "changed";

	SubtitleGridRowState state;
	state.selected = true;

	auto diff = BuildSubtitleGridDiffFromCommit(
		AssFile::COMMIT_DIAG_TEXT,
		7,
		8,
		&line,
		state,
		std::vector<std::string>{SubtitleGridColumnIdText});

	EXPECT_EQ(SubtitleGridDiffKind::RowsChanged, diff.kind);
	EXPECT_FALSE(diff.requires_full_refresh);
	ASSERT_EQ(1u, diff.upserted_rows.size());
	EXPECT_EQ(55, diff.upserted_rows[0].line_id);
	EXPECT_EQ(9, diff.upserted_rows[0].row_index);
	EXPECT_EQ("changed", diff.upserted_rows[0].text);
	EXPECT_TRUE(diff.upserted_rows[0].state.selected);
}

TEST(presentation_contract, builds_single_row_diff_for_timing_commit) {
	AssDialogue line;
	line.Id = 57;
	line.Row = 4;
	line.Start = 1200;
	line.End = 2400;
	line.Text = "timed";

	SubtitleGridRowState state;
	state.active = true;

	auto diff = BuildSubtitleGridDiffFromCommit(
		AssFile::COMMIT_DIAG_TIME,
		8,
		9,
		&line,
		state,
		std::vector<std::string>{
			SubtitleGridColumnIdStart,
			SubtitleGridColumnIdEnd,
			SubtitleGridColumnIdCps});

	EXPECT_EQ(SubtitleGridDiffKind::RowsChanged, diff.kind);
	EXPECT_FALSE(diff.requires_full_refresh);
	ASSERT_EQ(1u, diff.upserted_rows.size());
	EXPECT_EQ(57, diff.upserted_rows[0].line_id);
	EXPECT_EQ(4, diff.upserted_rows[0].row_index);
	EXPECT_EQ(1200, diff.upserted_rows[0].start_ms);
	EXPECT_EQ(2400, diff.upserted_rows[0].end_ms);
	EXPECT_EQ("timed", diff.upserted_rows[0].text);
	EXPECT_TRUE(diff.upserted_rows[0].state.active);
}

TEST(presentation_contract, timing_and_metadata_diffs_preserve_existing_full_refresh_semantics) {
	auto timing_diff = BuildSubtitleGridDiffFromCommit(
		AssFile::COMMIT_DIAG_TIME,
		11,
		12);
	EXPECT_EQ(SubtitleGridDiffKind::RowsChanged, timing_diff.kind);
	EXPECT_TRUE(timing_diff.requires_full_refresh);

	auto metadata_diff = BuildSubtitleGridDiffFromCommit(
		AssFile::COMMIT_DIAG_META,
		12,
		13);
	EXPECT_EQ(SubtitleGridDiffKind::RowsChanged, metadata_diff.kind);
	EXPECT_TRUE(metadata_diff.requires_full_refresh);
}
