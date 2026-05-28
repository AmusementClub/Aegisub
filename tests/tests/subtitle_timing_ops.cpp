#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/selection_controller.h"
#include "../../src/subtitle_timing_ops.h"

#include <memory>

namespace {

struct dialogue_list_fixture {
	std::vector<std::unique_ptr<AssDialogue>> storage;
	std::vector<AssDialogue *> ordered_events;

	AssDialogue *AddLine(int row, int start, int end) {
		storage.push_back(std::make_unique<AssDialogue>());
		auto *line = storage.back().get();
		line->Row = row;
		line->Start = start;
		line->End = end;
		ordered_events.push_back(line);
		return line;
	}
};

}

TEST(subtitle_timing_ops, adjoinable_selection_rejects_gapped_rows) {
	AssDialogue first;
	first.Row = 0;
	AssDialogue third;
	third.Row = 2;

	std::vector<AssDialogue *> selection = {&first, &third};

	EXPECT_FALSE(aegisub::subtitle_timing_ops::IsAdjoinableSelection(selection, 3));
}

TEST(subtitle_timing_ops, adjoinable_selection_accepts_single_and_full_file) {
	AssDialogue first;
	first.Row = 0;
	AssDialogue second;
	second.Row = 1;

	EXPECT_TRUE(aegisub::subtitle_timing_ops::IsAdjoinableSelection({&first}, 3));
	EXPECT_TRUE(aegisub::subtitle_timing_ops::IsAdjoinableSelection({&first, &second}, 2));
}

TEST(subtitle_timing_ops, adjoin_selection_updates_single_line_end_from_next_line) {
	dialogue_list_fixture fixture;
	auto *first = fixture.AddLine(0, 0, 1000);
	auto *second = fixture.AddLine(1, 1200, 2000);
	auto *third = fixture.AddLine(2, 2500, 3000);

	Selection selection = {second};

	ASSERT_TRUE(aegisub::subtitle_timing_ops::AdjoinSelection(fixture.ordered_events, selection, false));

	EXPECT_EQ(2500, static_cast<int>(second->End));
	EXPECT_EQ(1000, static_cast<int>(first->End));
	EXPECT_EQ(2500, static_cast<int>(third->Start));
}

TEST(subtitle_timing_ops, adjoin_selection_updates_internal_boundaries_for_multi_line_blocks) {
	dialogue_list_fixture fixture;
	auto *first = fixture.AddLine(0, 0, 1000);
	auto *second = fixture.AddLine(1, 1200, 2000);
	auto *third = fixture.AddLine(2, 2300, 3000);

	Selection selection = {second, third};

	ASSERT_TRUE(aegisub::subtitle_timing_ops::AdjoinSelection(fixture.ordered_events, selection, false));

	EXPECT_EQ(2300, static_cast<int>(second->End));
	EXPECT_EQ(3000, static_cast<int>(third->End));
	EXPECT_EQ(1000, static_cast<int>(first->End));
}

TEST(subtitle_timing_ops, shift_selection_to_start_time_moves_all_selected_lines_by_same_delta) {
	AssDialogue first;
	first.Start = 1000;
	first.End = 2000;
	AssDialogue second;
	second.Start = 3000;
	second.End = 4000;

	Selection selection = {&first, &second};
	agi::vfr::Framerate fps(10.); // 10 fps: frame 10 = 1000ms

	ASSERT_TRUE(aegisub::subtitle_timing_ops::ShiftSelectionToStartFrame(selection, &first, 7, fps));

	EXPECT_EQ(700, static_cast<int>(first.Start));
	EXPECT_EQ(1700, static_cast<int>(first.End));
	EXPECT_EQ(2700, static_cast<int>(second.Start));
	EXPECT_EQ(3700, static_cast<int>(second.End));
}

TEST(subtitle_timing_ops, snap_selection_to_video_range_preserves_non_conflicting_start_times) {
	AssDialogue early;
	early.Start = 600;
	early.End = 800;
	AssDialogue late;
	late.Start = 1200;
	late.End = 1500;

	Selection selection = {&early, &late};

	ASSERT_TRUE(aegisub::subtitle_timing_ops::SnapSelectionToVideoRange(selection, 1000, 1030, false));

	EXPECT_EQ(600, static_cast<int>(early.Start));
	EXPECT_EQ(1030, static_cast<int>(early.End));
	EXPECT_EQ(1000, static_cast<int>(late.Start));
	EXPECT_EQ(1030, static_cast<int>(late.End));
}

TEST(subtitle_timing_ops, compute_scene_snap_frame_range_handles_boundaries_and_exact_hits) {
	std::vector<int> keyframes = {10, 20, 30};

	auto before_first = aegisub::subtitle_timing_ops::ComputeSceneSnapFrameRange(keyframes, 5, 40);
	ASSERT_TRUE(before_first.has_value());
	EXPECT_EQ(0, before_first->start_frame);
	EXPECT_EQ(10, before_first->one_past_end_frame);

	auto exact_hit = aegisub::subtitle_timing_ops::ComputeSceneSnapFrameRange(keyframes, 20, 40);
	ASSERT_TRUE(exact_hit.has_value());
	EXPECT_EQ(20, exact_hit->start_frame);
	EXPECT_EQ(30, exact_hit->one_past_end_frame);

	auto after_last = aegisub::subtitle_timing_ops::ComputeSceneSnapFrameRange(keyframes, 35, 40);
	ASSERT_TRUE(after_last.has_value());
	EXPECT_EQ(30, after_last->start_frame);
	EXPECT_EQ(40, after_last->one_past_end_frame);
}

TEST(subtitle_timing_ops, apply_time_range_to_selection_overwrites_all_selected_lines) {
	AssDialogue first;
	first.Start = 100;
	first.End = 200;
	AssDialogue second;
	second.Start = 300;
	second.End = 400;

	Selection selection = {&first, &second};

	ASSERT_TRUE(aegisub::subtitle_timing_ops::ApplyTimeRangeToSelection(selection, 500, 900));

	EXPECT_EQ(500, static_cast<int>(first.Start));
	EXPECT_EQ(900, static_cast<int>(first.End));
	EXPECT_EQ(500, static_cast<int>(second.Start));
	EXPECT_EQ(900, static_cast<int>(second.End));
}
