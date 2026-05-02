#include <gtest/gtest.h>

#include "../../src/export_framerate_transform.h"

#include <libaegisub/vfr.h>

TEST(export_framerate_transform, dialogue_boundaries_preserve_start_and_end_frames) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);

	auto const transform = BuildAssFramerateTransform(source, destination, 5, 15);

	EXPECT_EQ(10, transform.new_start_ms);
	EXPECT_EQ(30, transform.new_end_ms);
	EXPECT_EQ(10, transform.old_ass_start_ms);
	EXPECT_EQ(20, transform.old_ass_end_ms);
}

TEST(export_framerate_transform, instant_mapping_uses_frame_identity_not_frame_interpolation) {
	auto const source = agi::vfr::Framerate({ 0, 10, 30, 60 });
	auto const destination = agi::vfr::Framerate(25.0);

	EXPECT_EQ(40, TransformFrameInstantTimeForExport(source, destination, 20));
}

TEST(export_framerate_transform, relative_start_tags_use_ass_storage_anchor) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 5, 15);

	EXPECT_EQ(50, TransformRelativeStartTagTimeForExport(source, destination, transform, 20));
}

TEST(export_framerate_transform, relative_end_tags_use_ass_storage_anchor) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 5, 15);

	EXPECT_EQ(10, TransformRelativeEndTagTimeForExport(source, destination, transform, 5));
}

TEST(export_framerate_transform, short_intervals_anchor_ass_tags_to_projected_transformed_dialogue) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 14, 15);

	EXPECT_EQ(30, transform.new_start_ms);
	EXPECT_EQ(30, transform.new_end_ms);
	EXPECT_EQ(10, transform.old_ass_start_ms);
	EXPECT_EQ(20, transform.old_ass_end_ms);
	EXPECT_EQ(30, transform.new_ass_start_ms);
	EXPECT_EQ(30, transform.new_ass_end_ms);
}

TEST(export_framerate_transform, karaoke_durations_follow_transformed_absolute_boundaries) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 5, 35);
	int old_accumulated_cs = 0;
	int new_accumulated_cs = 0;

	EXPECT_EQ(3, TransformKaraokeDurationForExport(source, destination, transform, 1, old_accumulated_cs, new_accumulated_cs));
	EXPECT_EQ(4, TransformKaraokeDurationForExport(source, destination, transform, 2, old_accumulated_cs, new_accumulated_cs));
	EXPECT_EQ(3, old_accumulated_cs);
	EXPECT_EQ(7, new_accumulated_cs);
}

TEST(export_framerate_transform, karaoke_start_tags_transform_absolute_boundaries) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 5, 55);
	int old_accumulated_cs = 2;
	int new_accumulated_cs = 4;

	EXPECT_EQ(7, TransformKaraokeStartForExport(source, destination, transform, 3, old_accumulated_cs, new_accumulated_cs));
	EXPECT_EQ(3, old_accumulated_cs);
	EXPECT_EQ(7, new_accumulated_cs);
	EXPECT_EQ(4, TransformKaraokeDurationForExport(source, destination, transform, 2, old_accumulated_cs, new_accumulated_cs));
	EXPECT_EQ(5, old_accumulated_cs);
	EXPECT_EQ(11, new_accumulated_cs);
}

TEST(export_framerate_transform, karaoke_start_tags_can_remain_before_line_start) {
	auto const source = agi::vfr::Framerate(100.0);
	auto const destination = agi::vfr::Framerate(50.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 105, 205);
	int old_accumulated_cs = 0;
	int new_accumulated_cs = 0;

	EXPECT_EQ(-3, TransformKaraokeStartForExport(source, destination, transform, -2, old_accumulated_cs, new_accumulated_cs));
	EXPECT_EQ(-2, old_accumulated_cs);
	EXPECT_EQ(-3, new_accumulated_cs);
}
