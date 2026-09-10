#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_io_core.h"
#include "../../src/presentation/subtitle_grid_projection.h"
#include "../../src/subtitle_grid_folding.h"
#include "../../src/subtitle_grid_ops.h"
#include "../../src/subtitle_grid_selection_policy.h"

#include <libaegisub/vfr.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace presentation = aegisub::presentation;
namespace policy = aegisub::subtitle_grid_selection_policy;

class subtitle_grid_folding : public ::testing::Test {
	protected:
	AssFile file;
	std::vector<AssDialogue *> lines;

	void SetUp() override {
		file.LoadDefault(false);
		for (int row = 0; row < 10; ++row) {
			auto *line = new AssDialogue;
			line->Row = row;
			line->Text = "line " + std::to_string(row);
			line->Start = row * 1000;
			line->End = (row + 1) * 1000;
			file.Events.push_back(*line);
			lines.push_back(line);
		}
	}

	void Mark(int row, std::string const& value, std::string const& key = "_aegi_folddata") {
		auto ids = lines[row]->ExtradataIds.get();
		ids.push_back(file.AddExtradata(key, value));
		lines[row]->ExtradataIds = std::move(ids);
	}

	[[nodiscard]] std::vector<std::string> Values(AssDialogue const& line, std::string const& key = "_aegi_folddata") const {
		std::vector<std::string> result;
		for (auto const& entry : file.GetExtradata(line.ExtradataIds.get())) {
			if (entry.key == key)
				result.push_back(entry.value);
		}
		return result;
	}

	std::vector<int> Visible() {
		std::vector<int> result;
		for (auto const& row : file.Folding().DisplayRows())
			result.push_back(row.source_row);
		return result;
	}

	void Delete(AssDialogue *line) {
		file.Events.erase_and_dispose(file.Events.iterator_to(*line), [](AssDialogue *deleted) { delete deleted; });
	}
};
}

TEST_F(subtitle_grid_folding, creates_serialized_boundaries_and_bidirectional_mapping) {
	Mark(2, "preserved", "other");
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	ASSERT_EQ(1u, folding.Groups().size());
	auto const& group = folding.Groups().front();
	EXPECT_EQ(2, group.start);
	EXPECT_EQ(6, group.end);
	EXPECT_TRUE(group.collapsed);
	EXPECT_EQ(std::vector<std::string>{"0;1;" + std::to_string(group.id)}, Values(*lines[2]));
	EXPECT_EQ(std::vector<std::string>{"1;1;" + std::to_string(group.id)}, Values(*lines[6]));
	EXPECT_EQ(std::vector<std::string>{"preserved"}, Values(*lines[2], "other"));
	EXPECT_EQ((std::vector<int>{0, 1, 2, 7, 8, 9}), Visible());
	EXPECT_EQ(3, folding.SourceToDisplay(7));
	EXPECT_EQ(7, folding.DisplayToSource(3));
	for (int source = 3; source <= 6; ++source)
		EXPECT_EQ(-1, folding.SourceToDisplay(source));
	EXPECT_EQ(-1, folding.SourceToDisplay(-1));
	EXPECT_EQ(-1, folding.SourceToDisplay(10));
	EXPECT_EQ(-1, folding.DisplayToSource(-1));
	EXPECT_EQ(-1, folding.DisplayToSource(6));
	auto const& entry = folding.DisplayRows()[2];
	EXPECT_EQ(lines[2], entry.dialogue);
	EXPECT_EQ(2, entry.display_row);
	EXPECT_TRUE(entry.is_fold_start);
	EXPECT_FALSE(entry.is_fold_end);
	EXPECT_TRUE(entry.is_collapsed);
	EXPECT_EQ(4, entry.hidden_count);
}

TEST_F(subtitle_grid_folding, expanded_and_adjacent_groups_keep_distinct_boundaries) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 1, 3, false));
	ASSERT_TRUE(folding.Create(file, 4, 6));
	EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 7, 8, 9}), Visible());
	EXPECT_TRUE(folding.DisplayRows()[1].is_fold_start);
	EXPECT_FALSE(folding.DisplayRows()[1].is_collapsed);
	EXPECT_TRUE(folding.DisplayRows()[3].is_fold_end);
	EXPECT_EQ(0, folding.DisplayRows()[3].hidden_count);
	ASSERT_TRUE(folding.SetAllCollapsed(file, true));
	EXPECT_EQ((std::vector<int>{0, 1, 4, 7, 8, 9}), Visible());
	EXPECT_FALSE(folding.SetAllCollapsed(file, true));
	ASSERT_TRUE(folding.SetAllCollapsed(file, false));
	EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), Visible());
	for (int row = 0; row < 10; ++row) {
		EXPECT_EQ(row, folding.SourceToDisplay(row));
		EXPECT_EQ(row, folding.DisplayToSource(row));
	}
}

TEST_F(subtitle_grid_folding, rejects_nested_crossing_shared_boundary_and_invalid_creation_ranges) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 3, 6));
	for (auto const& range : std::vector<std::pair<int, int>>{{3, 6}, {4, 5}, {2, 7}, {1, 4}, {5, 8}, {1, 3}, {6, 8}, {4, 4}, {6, 3}, {-1, 2}, {8, 10}}) {
		SCOPED_TRACE(std::to_string(range.first) + "," + std::to_string(range.second));
		EXPECT_FALSE(folding.CanCreate(range.first, range.second));
		EXPECT_FALSE(folding.Create(file, range.first, range.second));
	}
	EXPECT_TRUE(folding.CanCreate(1, 2));
	EXPECT_TRUE(folding.CanCreate(7, 8));
	EXPECT_EQ(1u, folding.Groups().size());
	EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 7, 8, 9}), Visible());
}

TEST_F(subtitle_grid_folding, parses_stable_ids_and_start_state_wins_without_rewriting_input) {
	Mark(2, "0;1;18446744073709551615");
	Mark(5, "1;0;18446744073709551615");
	auto& folding = file.Folding();
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(std::numeric_limits<uint64_t>::max(), folding.Groups().front().id);
	EXPECT_TRUE(folding.Groups().front().collapsed);
	EXPECT_EQ((std::vector<int>{0, 1, 2, 6, 7, 8, 9}), Visible());
	EXPECT_EQ(std::vector<std::string>{"1;0;18446744073709551615"}, Values(*lines[5]));
	ASSERT_TRUE(folding.SetCollapsed(file, 4, false));
	EXPECT_EQ(std::vector<std::string>{"0;0;18446744073709551615"}, Values(*lines[2]));
	EXPECT_EQ(std::vector<std::string>{"1;0;18446744073709551615"}, Values(*lines[5]));
}

TEST_F(subtitle_grid_folding, malformed_missing_duplicate_and_reversed_boundaries_remain_visible) {
	std::vector<std::vector<std::pair<int, std::string>>> cases = {
		{{2, "0;1;42"}},
		{{5, "1;1;42"}},
		{{2, "0;1;42"}, {3, "0;1;42"}, {5, "1;1;42"}},
		{{2, "0;1;42"}, {4, "1;1;42"}, {5, "1;1;42"}},
		{{2, "1;1;42"}, {5, "0;1;42"}},
		{{2, "0;1;42"}, {2, "1;1;42"}},
		{{2, "2;1;42"}, {5, "1;1;42"}},
		{{2, "0;x;42"}, {5, "1;1;42"}},
		{{2, "0;1;42x"}, {5, "1;1;42x"}},
		{{2, "0;1;18446744073709551616"}, {5, "1;1;18446744073709551616"}},
		{{2, "0;1;-1"}, {5, "1;1;-1"}},
		{{2, "0;1;42;extra"}, {5, "1;1;42"}},
	};
	for (auto const& markers : cases) {
		for (auto *line : lines)
			line->ExtradataIds = std::vector<uint32_t>{};
		for (auto const& [row, marker] : markers)
			Mark(row, marker);
		SCOPED_TRACE(markers.front().second);
		file.Folding().Rebuild(file);
		EXPECT_TRUE(file.Folding().Groups().empty());
		EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), Visible());
		for (auto const& [row, marker] : markers) {
			auto values = Values(*lines[row]);
			EXPECT_NE(values.end(), std::ranges::find(values, marker));
		}
	}
}

TEST_F(subtitle_grid_folding, crossing_and_nested_components_are_invalid_without_discarding_separate_group) {
	for (bool nested : {false, true}) {
		for (auto *line : lines)
			line->ExtradataIds = std::vector<uint32_t>{};
		Mark(1, "0;1;1");
		Mark(5, "1;1;1");
		Mark(3, "0;1;2");
		Mark(nested ? 4 : 6, "1;1;2");
		Mark(7, "0;1;3");
		Mark(9, "1;1;3");
		file.Folding().Rebuild(file);
		ASSERT_EQ(1u, file.Folding().Groups().size());
		EXPECT_EQ(3u, file.Folding().Groups().front().id);
		EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}), Visible());
	}
}

TEST_F(subtitle_grid_folding, temporary_reveal_keeps_persisted_state_and_does_not_commit) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	int commits = 0;
	auto connection = agi::signal::Connection(file.AddUndoManager([&](AssFileCommit) { ++commits; }));
	auto const boundary = Values(*lines[2]);
	auto const extra_count = file.Extradata.size();
	EXPECT_FALSE(folding.EnsureVisible(2));
	EXPECT_FALSE(folding.EnsureVisible(-1));
	ASSERT_TRUE(folding.EnsureVisible(5));
	EXPECT_EQ(5, folding.SourceToDisplay(5));
	EXPECT_EQ(lines[5], folding.DisplayRows()[5].dialogue);
	EXPECT_FALSE(folding.DisplayRows()[2].is_collapsed);
	EXPECT_TRUE(folding.Groups().front().collapsed);
	EXPECT_EQ(boundary, Values(*lines[2]));
	EXPECT_EQ(extra_count, file.Extradata.size());
	EXPECT_EQ(0, commits);
	EXPECT_FALSE(folding.EnsureVisible(5));
	folding.Rebuild(file);
	EXPECT_EQ(5, folding.SourceToDisplay(5));
	ASSERT_TRUE(folding.Toggle(file, 4));
	EXPECT_EQ(-1, folding.SourceToDisplay(5));
	EXPECT_EQ(boundary, Values(*lines[2]));
	ASSERT_TRUE(folding.EnsureVisible(5));
	folding.Rebuild(file, true);
	EXPECT_EQ(-1, folding.SourceToDisplay(5));
}

TEST_F(subtitle_grid_folding, remove_and_clear_preserve_subtitles_and_unrelated_extradata) {
	Mark(2, "retained", "other");
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 5));
	ASSERT_TRUE(folding.Create(file, 7, 9));
	ASSERT_TRUE(folding.Remove(file, 4));
	EXPECT_TRUE(Values(*lines[2]).empty());
	EXPECT_TRUE(Values(*lines[5]).empty());
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(7, folding.Groups().front().start);
	Mark(0, "invalid-marker");
	folding.Rebuild(file);
	ASSERT_TRUE(folding.Clear(file));
	EXPECT_FALSE(folding.Clear(file));
	EXPECT_TRUE(folding.Groups().empty());
	EXPECT_TRUE(Values(*lines[0]).empty());
	EXPECT_EQ(std::vector<std::string>{"retained"}, Values(*lines[2], "other"));
	EXPECT_EQ("line 5", lines[5]->Text.get());
	EXPECT_EQ(10, std::distance(file.Events.begin(), file.Events.end()));
}

TEST_F(subtitle_grid_folding, shift_click_drag_and_ctrl_extension_include_hidden_source_rows) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	policy::MouseSelectionInput input;
	input.row_count = 10;
	input.target_row = folding.DisplayToSource(3);
	input.anchor_row = folding.DisplayToSource(1);
	input.selected_rows = {0, 9};
	input.click = true;
	input.modifiers.shift = true;
	auto shift = policy::PlanMouseSelection(input);
	EXPECT_EQ((std::vector<int>{1, 2, 3, 4, 5, 6, 7}), shift.selected_rows);
	input.modifiers.ctrl = true;
	auto extend = policy::PlanMouseSelection(input);
	EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 9}), extend.selected_rows);
	input.click = false;
	input.dragging = true;
	input.modifiers = {};
	std::swap(input.target_row, input.anchor_row);
	auto drag = policy::PlanMouseSelection(input);
	EXPECT_EQ((std::vector<int>{1, 2, 3, 4, 5, 6, 7}), drag.selected_rows);
	input.click = true;
	input.dragging = false;
	input.target_row = folding.DisplayToSource(2);
	auto plain = policy::PlanMouseSelection(input);
	EXPECT_EQ(std::vector<int>{2}, plain.selected_rows);
}

TEST_F(subtitle_grid_folding, folded_window_pages_by_display_rows_and_preserves_source_identity_and_state) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	presentation::VisibleSubtitleRowsRequest request;
	request.first_row = 2;
	request.row_count = 3;
	request.column_ids = {presentation::SubtitleGridColumnIdText};
	std::vector<int> resolved;
	auto window = presentation::BuildFoldedSubtitleGridWindow(folding.DisplayRows(), request, 19, [&](AssDialogue const& line) {
		resolved.push_back(line.Row);
		presentation::SubtitleGridRowState state;
		state.selected = &line == lines[2];
		state.active = &line == lines[7];
		return state;
	});
	EXPECT_EQ(19u, window.revision);
	EXPECT_EQ(2, window.first_row);
	EXPECT_EQ(6, window.total_rows);
	ASSERT_EQ(3u, window.rows.size());
	EXPECT_EQ((std::vector<int>{2, 7, 8}), resolved);
	EXPECT_EQ(2, window.rows[0].row_index);
	EXPECT_EQ(7, window.rows[1].row_index);
	EXPECT_EQ(8, window.rows[2].row_index);
	EXPECT_EQ(lines[7]->Id, window.rows[1].line_id);
	EXPECT_EQ("line 7", window.rows[1].text);
	EXPECT_TRUE(window.rows[0].state.selected);
	EXPECT_TRUE(window.rows[1].state.active);
	EXPECT_FALSE(window.rows[2].state.active);
	EXPECT_EQ(0, window.rows[1].start_ms);
	EXPECT_TRUE(window.rows[1].style.empty());
}

TEST_F(subtitle_grid_folding, folded_window_clamps_negative_oversized_and_empty_requests) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 8));
	presentation::VisibleSubtitleRowsRequest request;
	request.first_row = -3;
	request.row_count = std::numeric_limits<int>::max();
	auto window = presentation::BuildFoldedSubtitleGridWindow(folding.DisplayRows(), request, 1);
	EXPECT_EQ(0, window.first_row);
	EXPECT_EQ(4u, window.rows.size());
	request.first_row = 50;
	window = presentation::BuildFoldedSubtitleGridWindow(folding.DisplayRows(), request, 2);
	EXPECT_EQ(4, window.first_row);
	EXPECT_TRUE(window.rows.empty());
	request.first_row = 1;
	request.row_count = -1;
	window = presentation::BuildFoldedSubtitleGridWindow(folding.DisplayRows(), request, 3);
	EXPECT_EQ(1, window.first_row);
	EXPECT_TRUE(window.rows.empty());
	AssFile empty;
	window = presentation::BuildFoldedSubtitleGridWindow(empty.Folding().DisplayRows(), request, 4);
	EXPECT_EQ(0, window.total_rows);
	EXPECT_TRUE(window.rows.empty());
}

TEST_F(subtitle_grid_folding, structural_commit_includes_inserted_members_and_preserves_deleted_interior) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	auto *inserted = new AssDialogue;
	file.Events.insert(file.Events.iterator_to(*lines[4]), *inserted);
	file.Commit("insert", AssFile::COMMIT_DIAG_ADDREM);
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(7, folding.Groups().front().end);
	EXPECT_EQ(-1, folding.SourceToDisplay(4));
	Delete(lines[3]);
	file.Commit("delete member", AssFile::COMMIT_DIAG_ADDREM);
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(6, folding.Groups().front().end);
	EXPECT_EQ((std::vector<int>{0, 1, 2, 7, 8, 9}), Visible());
}

TEST_F(subtitle_grid_folding, deleting_boundary_clears_surviving_marker_before_undo_snapshot) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	int snapshot_type = 0;
	std::vector<std::string> snapshot_values;
	auto connection = agi::signal::Connection(file.AddUndoManager([&](AssFileCommit commit) {
		snapshot_type = commit.type;
		snapshot_values = Values(*lines[6]);
	}));
	Delete(lines[2]);
	file.Commit("delete boundary", AssFile::COMMIT_DIAG_ADDREM);
	EXPECT_TRUE(folding.Groups().empty());
	EXPECT_TRUE(snapshot_values.empty());
	EXPECT_NE(0, snapshot_type & AssFile::COMMIT_FOLD);
	EXPECT_TRUE(Values(*lines[6]).empty());
	EXPECT_EQ(9u, folding.DisplayRows().size());
}

TEST_F(subtitle_grid_folding, deleting_end_boundary_clears_start_marker_and_reveals_remaining_members) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	Delete(lines[6]);
	file.Commit("delete end boundary", AssFile::COMMIT_DIAG_ADDREM);
	EXPECT_TRUE(folding.Groups().empty());
	EXPECT_TRUE(Values(*lines[2]).empty());
	EXPECT_EQ(5, folding.SourceToDisplay(5));
	EXPECT_EQ(lines[5], folding.DisplayRows()[5].dialogue);
}

TEST_F(subtitle_grid_folding, moving_member_outside_boundaries_invalidates_group) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	file.Events.splice(file.Events.end(), file.Events, file.Events.iterator_to(*lines[4]));
	file.Commit("move member outside group", AssFile::COMMIT_ORDER);
	EXPECT_TRUE(folding.Groups().empty());
	EXPECT_TRUE(Values(*lines[2]).empty());
	EXPECT_TRUE(Values(*lines[6]).empty());
	EXPECT_EQ(10u, folding.DisplayRows().size());
	EXPECT_EQ(lines[4], folding.DisplayRows().back().dialogue);
}

TEST_F(subtitle_grid_folding, moving_whole_group_preserves_it_but_reordered_members_clear_markers) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 5));
	auto const id = folding.Groups().front().id;
	ASSERT_TRUE(aegisub::subtitle_grid_ops::MoveSelectionDown(file.Events, {lines[2], lines[3], lines[4], lines[5]}));
	file.Commit("move group", AssFile::COMMIT_ORDER);
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(id, folding.Groups().front().id);
	EXPECT_EQ(3, folding.Groups().front().start);
	EXPECT_EQ(6, folding.Groups().front().end);
	ASSERT_TRUE(aegisub::subtitle_grid_ops::SwapSelection({lines[3], lines[4]}));
	file.Commit("reorder members", AssFile::COMMIT_ORDER);
	EXPECT_TRUE(folding.Groups().empty());
	EXPECT_TRUE(Values(*lines[2]).empty());
	EXPECT_TRUE(Values(*lines[5]).empty());
}

TEST_F(subtitle_grid_folding, sorting_foreign_line_between_boundaries_invalidates_group) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	lines[8]->Start = 4500;
	file.Sort();
	file.Commit("sort", AssFile::COMMIT_ORDER);
	EXPECT_TRUE(folding.Groups().empty());
	EXPECT_TRUE(Values(*lines[2]).empty());
	EXPECT_TRUE(Values(*lines[6]).empty());
	EXPECT_EQ(10u, folding.DisplayRows().size());
}

TEST_F(subtitle_grid_folding, complete_copies_receive_new_group_ids_and_partial_copies_keep_only_unrelated_data) {
	Mark(2, "retained", "other");
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 4));
	auto const original_id = folding.Groups().front().id;
	std::vector<AssDialogue *> copies;
	for (int row = 2; row <= 4; ++row) {
		auto *copy = new AssDialogue(*lines[row]);
		SubtitleGridFolding::StripMarkers(file, *copy);
		file.Events.push_back(*copy);
		copies.push_back(copy);
	}
	SubtitleGridFolding::AddCopiedGroup(file, *copies.front(), *copies.back(), true);
	file.Commit("duplicate group", AssFile::COMMIT_DIAG_ADDREM);
	ASSERT_EQ(2u, folding.Groups().size());
	EXPECT_EQ(original_id, folding.Groups()[0].id);
	EXPECT_NE(original_id, folding.Groups()[1].id);
	EXPECT_EQ(10, folding.Groups()[1].start);
	EXPECT_EQ(12, folding.Groups()[1].end);
	EXPECT_EQ(-1, folding.SourceToDisplay(12));
	EXPECT_EQ(std::vector<std::string>{"retained"}, Values(*copies.front(), "other"));
	auto *partial = new AssDialogue(*lines[2]);
	EXPECT_TRUE(SubtitleGridFolding::StripMarkers(file, *partial));
	file.Events.push_back(*partial);
	file.Commit("duplicate one boundary", AssFile::COMMIT_DIAG_ADDREM);
	EXPECT_EQ(2u, folding.Groups().size());
	EXPECT_TRUE(Values(*partial).empty());
	EXPECT_EQ(std::vector<std::string>{"retained"}, Values(*partial, "other"));
	EXPECT_GE(folding.SourceToDisplay(13), 0);
}

TEST_F(subtitle_grid_folding, undo_redo_snapshots_restore_boundaries_state_and_current_dialogue_pointers) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 2, 6));
	std::vector<AssDialogueBase> collapsed;
	for (auto const& line : file.Events)
		collapsed.push_back(line);
	auto const collapsed_extra = file.Extradata;
	ASSERT_TRUE(folding.SetCollapsed(file, 4, false));
	std::vector<AssDialogueBase> expanded;
	for (auto const& line : file.Events)
		expanded.push_back(line);
	auto const expanded_extra = file.Extradata;
	auto restore = [&](std::vector<AssDialogueBase> const& snapshot, std::vector<ExtradataEntry> const& extradata) {
		file.Events.clear_and_dispose([](AssDialogue *line) { delete line; });
		for (auto const& line : snapshot)
			file.Events.push_back(*new AssDialogue(line));
		file.Extradata = extradata;
		file.Commit("restore snapshot", AssFile::COMMIT_NEW);
	};
	restore(collapsed, collapsed_extra);
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(-1, folding.SourceToDisplay(5));
	EXPECT_EQ(collapsed[2].Id, folding.DisplayRows()[2].dialogue->Id);
	EXPECT_EQ(&*std::next(file.Events.begin(), 2), folding.DisplayRows()[2].dialogue);
	ASSERT_TRUE(folding.EnsureVisible(5));
	restore(expanded, expanded_extra);
	EXPECT_FALSE(folding.Groups().front().collapsed);
	EXPECT_EQ(5, folding.SourceToDisplay(5));
	EXPECT_EQ(&*std::next(file.Events.begin(), 5), folding.DisplayRows()[5].dialogue);
	restore(collapsed, collapsed_extra);
	EXPECT_EQ(-1, folding.SourceToDisplay(5));
}

TEST_F(subtitle_grid_folding, ass_save_reload_preserves_groups_without_changing_dialogue_order_or_text) {
	ASSERT_TRUE(file.Folding().Create(file, 2, 6));
	auto const group_id = file.Folding().Groups().front().id;
	auto path = std::filesystem::temp_directory_path() / "aegisub-grid-folding-roundtrip.ass";
	AssWriteOptions options;
	options.write_extradata = true;
	WriteAssFileForCore(&file, path, agi::vfr::Framerate(), "UTF-8", options);
	AssFile restored = ReadAssFileForCore(path, "UTF-8");
	std::filesystem::remove(path);
	auto& folding = restored.Folding();
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(group_id, folding.Groups().front().id);
	EXPECT_EQ(2, folding.Groups().front().start);
	EXPECT_EQ(6, folding.Groups().front().end);
	EXPECT_TRUE(folding.Groups().front().collapsed);
	EXPECT_EQ(6u, folding.DisplayRows().size());
	int row = 0;
	for (auto const& line : restored.Events) {
		EXPECT_EQ("line " + std::to_string(row), line.Text.get());
		++row;
	}
	EXPECT_EQ(10, row);
}
