#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_time_projection.h"

#include <libaegisub/vfr.h>

TEST(ass_time_projection, generic_end_time_projects_down_for_ass_storage) {
	AssDialogue line;
	line.Comment = false;
	line.Layer = 2;
	line.Start = 14189;
	line.End = 1059268;
	line.Style = "Default";
	line.Actor = "Actor";
	line.Margin = { { 1, 2, 3 } };
	line.Effect = "Effect";
	line.Text = "Hello";

	EXPECT_EQ(
		"Dialogue: 2,0:00:14.19,0:17:39.26,Default,Actor,1,2,3,Effect,Hello",
		SerializeAssDialogueForStorage(line));
}

TEST(ass_time_projection, frame_safe_times_preserve_100fps_snap_semantics) {
	auto const fps = agi::vfr::Framerate(100.0);

	EXPECT_EQ(10, ProjectAssTimeForStorage(5, AssStorageTimeBoundary::Start, &fps));
	EXPECT_EQ(20, ProjectAssTimeForStorage(15, AssStorageTimeBoundary::End, &fps));
}

TEST(ass_time_projection, non_canonical_times_preserve_100fps_frame_semantics_when_possible) {
	auto const fps = agi::vfr::Framerate(100.0);

	EXPECT_EQ(20, ProjectAssTimeForStorage(19, AssStorageTimeBoundary::Start, &fps));
	EXPECT_EQ(20, ProjectAssTimeForStorage(17, AssStorageTimeBoundary::End, &fps));
}

TEST(ass_time_projection, frame_safe_ties_use_boundary_conservative_candidate) {
	auto const fps = agi::vfr::Framerate(50.0);

	EXPECT_EQ(20, ProjectAssTimeForStorage(15, AssStorageTimeBoundary::Start, &fps));
	EXPECT_EQ(30, ProjectAssTimeForStorage(35, AssStorageTimeBoundary::End, &fps));
}

TEST(ass_time_projection, serializes_frame_safe_dialogue_using_projection_when_fps_is_available) {
	auto const fps = agi::vfr::Framerate(100.0);

	AssDialogue line;
	line.Comment = false;
	line.Layer = 0;
	line.Start = 5;
	line.End = 15;
	line.Style = "Default";
	line.Text = "frame";

	EXPECT_EQ(
		"Dialogue: 0,0:00:00.01,0:00:00.02,Default,,0,0,0,,frame",
		SerializeAssDialogueForStorage(line, &fps));
}

TEST(ass_time_projection, short_unrepresentable_intervals_expand_to_a_non_empty_ass_bucket) {
	AssDialogue line;
	line.Start = 14;
	line.End = 15;
	line.Style = "Default";
	line.Text = "short";

	EXPECT_EQ(
		"Dialogue: 0,0:00:00.01,0:00:00.02,Default,,0,0,0,,short",
		SerializeAssDialogueForStorage(line));
}

TEST(ass_time_projection, short_boundary_crossing_intervals_project_to_a_single_ass_bucket) {
	auto const projected = ProjectAssDialogueTimesForStorage(agi::Time(19), agi::Time(21));

	EXPECT_EQ(20, projected.first);
	EXPECT_EQ(30, projected.second);
}

TEST(ass_time_projection, projected_visibility_uses_storage_interval_not_original_ms_interval) {
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForStorage(agi::Time(18), agi::Time(19), 19));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForStorage(agi::Time(18), agi::Time(19), 20));
}

TEST(ass_dialogue, exact_millisecond_dialogue_text_roundtrips_through_parser) {
	AssDialogue line;
	line.Comment = false;
	line.Layer = 0;
	line.Start = 18497;
	line.End = 20499;
	line.Style = "Default";
	line.Text = "exact";

	AssDialogue parsed(line.GetEntryData(
		line.Start.GetAssFormatted(true),
		line.End.GetAssFormatted(true)));

	EXPECT_EQ(18497, parsed.Start.GetMillisecond());
	EXPECT_EQ(20499, parsed.End.GetMillisecond());
	EXPECT_EQ("exact", parsed.Text.get());
}
