#include <gtest/gtest.h>

#include "../../src/time_display_mode.h"

#include <libaegisub/fs.h>
#include <libaegisub/vfr.h>

TEST(time_display_mode, defaults_to_ass_for_ass_and_exact_for_other_formats) {
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.ass")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.ASS")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.ssa")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Exact, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.Srt")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Exact, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.srt")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile({}));
}

TEST(time_display_mode, ass_display_reuses_dialogue_storage_projection) {
	auto const fps = agi::vfr::Framerate(100.0);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(5), agi::Time(15), SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(10, displayed.first);
	EXPECT_EQ(20, displayed.second);
	EXPECT_EQ("0:00:00.01", FormatTimeForDisplay(displayed.first, SubtitleTimeDisplayMode::Ass));
}

TEST(time_display_mode, exact_display_preserves_internal_milliseconds) {
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(1234), agi::Time(2345), SubtitleTimeDisplayMode::Exact);

	EXPECT_EQ(1234, displayed.first);
	EXPECT_EQ(2345, displayed.second);
	EXPECT_EQ("0:00:01.234", FormatTimeForDisplay(displayed.first, SubtitleTimeDisplayMode::Exact));
}

TEST(time_display_mode, duration_follows_selected_display_semantics) {
	auto const fps = agi::vfr::Framerate(100.0);

	EXPECT_EQ(10, GetDurationForDisplay(agi::Time(5), agi::Time(15), SubtitleTimeDisplayMode::Ass, &fps));
	EXPECT_EQ(10, GetDurationForDisplay(agi::Time(5), agi::Time(15), SubtitleTimeDisplayMode::Exact));
	EXPECT_EQ(10, GetDurationForDisplay(agi::Time(14), agi::Time(15), SubtitleTimeDisplayMode::Ass, &fps));
	EXPECT_EQ(1, GetDurationForDisplay(agi::Time(14), agi::Time(15), SubtitleTimeDisplayMode::Exact));
}

TEST(time_display_mode, ass_duration_edit_keeps_existing_internal_end_for_same_display_duration) {
	auto const fps = agi::vfr::Framerate(100.0);

	auto const end = GetEndTimeForDisplayedDuration(
		agi::Time(14),
		agi::Time(40),
		agi::Time(20),
		SubtitleTimeDisplayMode::Ass,
		&fps);

	EXPECT_EQ(40, end.GetMillisecond());
}

TEST(time_display_mode, ass_duration_edit_preserves_current_displayed_start_when_representable) {
	auto const fps = agi::vfr::Framerate(100.0);

	auto const end = GetEndTimeForDisplayedDuration(
		agi::Time(14),
		agi::Time(40),
		agi::Time(10),
		SubtitleTimeDisplayMode::Ass,
		&fps);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(14), end, SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(30, end.GetMillisecond());
	EXPECT_EQ(20, displayed.first);
	EXPECT_EQ(30, displayed.second);
}

TEST(time_display_mode, ass_duration_edit_falls_back_to_canonical_start_when_collapsed_start_cannot_be_preserved) {
	auto const fps = agi::vfr::Framerate(100.0);

	auto const end = GetEndTimeForDisplayedDuration(
		agi::Time(14),
		agi::Time(15),
		agi::Time(20),
		SubtitleTimeDisplayMode::Ass,
		&fps);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(14), end, SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(40, end.GetMillisecond());
	EXPECT_EQ(20, displayed.first);
	EXPECT_EQ(40, displayed.second);
}
