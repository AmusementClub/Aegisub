#include <main.h>

#include "../../src/video_zoom.h"

TEST(video_zoom, detached_wheel_snaps_to_nearest_preset_before_stepping) {
	EXPECT_DOUBLE_EQ(1.0, AdvanceDetachedVideoZoomByWheel(1.03, 1, 24));
	EXPECT_DOUBLE_EQ(1.0, AdvanceDetachedVideoZoomByWheel(1.03, -1, 24));
}

TEST(video_zoom, detached_wheel_continues_stepping_after_initial_snap) {
	EXPECT_DOUBLE_EQ(1.125, AdvanceDetachedVideoZoomByWheel(1.03, 2, 24));
	EXPECT_DOUBLE_EQ(0.875, AdvanceDetachedVideoZoomByWheel(1.03, -2, 24));
}

TEST(video_zoom, detached_wheel_clamps_to_combo_preset_range) {
	EXPECT_DOUBLE_EQ(3.0, AdvanceDetachedVideoZoomByWheel(3.4, 1, 24));
	EXPECT_DOUBLE_EQ(0.125, AdvanceDetachedVideoZoomByWheel(0.05, -1, 24));
}

TEST(video_zoom, attached_style_preset_steps_are_preserved_once_aligned) {
	EXPECT_DOUBLE_EQ(1.125, AdvanceDetachedVideoZoomByWheel(1.0, 1, 24));
	EXPECT_DOUBLE_EQ(0.875, AdvanceDetachedVideoZoomByWheel(1.0, -1, 24));
}
