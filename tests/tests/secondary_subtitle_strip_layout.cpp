#include "../../src/secondary_subtitle_strip_layout.h"
#include "../../src/secondary_subtitle_presentation_demand.h"

#include <gtest/gtest.h>

TEST(secondary_subtitle_strip_layout, empty_content_returns_zero_layout) {
	auto layout = BuildSecondarySubtitleStripLayout(0, 180, 0);

	EXPECT_EQ(0, layout.source_top);
	EXPECT_EQ(0, layout.source_height);
	EXPECT_EQ(0, layout.max_scroll_offset_y);
	EXPECT_EQ(0, layout.clamped_scroll_offset_y);
}

TEST(secondary_subtitle_strip_layout, default_view_is_anchored_to_bottom) {
	auto layout = BuildSecondarySubtitleStripLayout(720, 180, 0);

	EXPECT_EQ(540, layout.source_top);
	EXPECT_EQ(180, layout.source_height);
	EXPECT_EQ(540, layout.max_scroll_offset_y);
	EXPECT_EQ(0, layout.clamped_scroll_offset_y);
}

TEST(secondary_subtitle_strip_layout, scrolling_up_moves_view_toward_top) {
	auto layout = BuildSecondarySubtitleStripLayout(720, 180, 60);

	EXPECT_EQ(480, layout.source_top);
	EXPECT_EQ(180, layout.source_height);
	EXPECT_EQ(540, layout.max_scroll_offset_y);
	EXPECT_EQ(60, layout.clamped_scroll_offset_y);
}

TEST(secondary_subtitle_strip_layout, scroll_offset_is_clamped_to_valid_range) {
	auto layout = BuildSecondarySubtitleStripLayout(720, 180, 9999);

	EXPECT_EQ(0, layout.source_top);
	EXPECT_EQ(180, layout.source_height);
	EXPECT_EQ(540, layout.max_scroll_offset_y);
	EXPECT_EQ(540, layout.clamped_scroll_offset_y);
}

TEST(secondary_subtitle_strip_layout, visible_source_height_is_clamped_to_content_height) {
	auto layout = BuildSecondarySubtitleStripLayout(120, 180, 40);

	EXPECT_EQ(0, layout.source_top);
	EXPECT_EQ(120, layout.source_height);
	EXPECT_EQ(0, layout.max_scroll_offset_y);
	EXPECT_EQ(0, layout.clamped_scroll_offset_y);
}

TEST(secondary_subtitle_strip_layout, bottom_anchored_view_maps_to_bottom_thumb_position) {
	auto layout = BuildSecondarySubtitleStripLayout(720, 180, 0);

	EXPECT_EQ(540, SecondarySubtitleStripThumbPositionFromScrollOffset(
		layout.clamped_scroll_offset_y,
		layout.max_scroll_offset_y));
}

TEST(secondary_subtitle_strip_layout, topmost_view_maps_to_top_thumb_position) {
	auto layout = BuildSecondarySubtitleStripLayout(720, 180, 540);

	EXPECT_EQ(0, SecondarySubtitleStripThumbPositionFromScrollOffset(
		layout.clamped_scroll_offset_y,
		layout.max_scroll_offset_y));
}

TEST(secondary_subtitle_strip_layout, thumb_position_round_trips_back_to_scroll_offset) {
	auto layout = BuildSecondarySubtitleStripLayout(720, 180, 160);
	int thumb_position = SecondarySubtitleStripThumbPositionFromScrollOffset(
		layout.clamped_scroll_offset_y,
		layout.max_scroll_offset_y);

	EXPECT_EQ(
		layout.clamped_scroll_offset_y,
		SecondarySubtitleStripScrollOffsetFromThumbPosition(thumb_position, layout.max_scroll_offset_y));
}

TEST(secondary_subtitle_strip_layout, attached_box_is_visible_only_in_attached_mode) {
	EXPECT_TRUE(ShouldShowSecondarySubtitleStrip(true, true, false, false));
	EXPECT_FALSE(ShouldShowSecondarySubtitleStrip(true, true, false, true));
}

TEST(secondary_subtitle_strip_layout, detached_box_is_visible_only_in_detached_mode) {
	EXPECT_FALSE(ShouldShowSecondarySubtitleStrip(true, true, true, false));
	EXPECT_TRUE(ShouldShowSecondarySubtitleStrip(true, true, true, true));
}

TEST(secondary_subtitle_strip_layout, secondary_strip_requires_video_and_enable_option) {
	EXPECT_FALSE(ShouldShowSecondarySubtitleStrip(false, true, false, false));
	EXPECT_FALSE(ShouldShowSecondarySubtitleStrip(true, false, false, false));
}

TEST(secondary_subtitle_presentation_demand, ignores_null_and_duplicate_presenters) {
	SecondarySubtitlePresentationDemand demand;
	int presenter = 0;

	EXPECT_EQ(SecondarySubtitlePresentationDemandChange::None, demand.Set(nullptr, true));
	EXPECT_FALSE(demand.HasDemand());
	EXPECT_EQ(SecondarySubtitlePresentationDemandChange::BecameDemanded, demand.Set(&presenter, true));
	EXPECT_TRUE(demand.HasDemand());
	EXPECT_EQ(SecondarySubtitlePresentationDemandChange::None, demand.Set(&presenter, true));
	EXPECT_EQ(SecondarySubtitlePresentationDemandChange::BecameIdle, demand.Set(&presenter, false));
	EXPECT_FALSE(demand.HasDemand());
	EXPECT_EQ(SecondarySubtitlePresentationDemandChange::None, demand.Set(&presenter, false));
}

TEST(secondary_subtitle_presentation_demand, remains_demanded_during_presenter_handoff) {
	SecondarySubtitlePresentationDemand demand;
	int attached_presenter = 0;
	int detached_presenter = 0;

	EXPECT_EQ(
		SecondarySubtitlePresentationDemandChange::BecameDemanded,
		demand.Set(&attached_presenter, true));
	EXPECT_EQ(
		SecondarySubtitlePresentationDemandChange::None,
		demand.Set(&detached_presenter, true));
	EXPECT_EQ(
		SecondarySubtitlePresentationDemandChange::None,
		demand.Set(&attached_presenter, false));
	EXPECT_TRUE(demand.HasDemand());
	EXPECT_EQ(
		SecondarySubtitlePresentationDemandChange::BecameIdle,
		demand.Set(&detached_presenter, false));
	EXPECT_FALSE(demand.HasDemand());
}
