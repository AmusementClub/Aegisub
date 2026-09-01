#include <main.h>

#include "../../src/visual_tool_roi_press_policy.h"

namespace {
using visual_tool_roi_press_policy::PressIsOnFrame;
using visual_tool_roi_press_policy::PressMayStartBand;
}

TEST(visual_tool_roi_press_policy, interior_presses_are_on_frame) {
	EXPECT_TRUE(PressIsOnFrame(0.f, 0.f, 1920, 1080));
	EXPECT_TRUE(PressIsOnFrame(960.f, 540.f, 1920, 1080));
	// Closed rectangle: the far edge counts, or the last row and column stop
	// being reachable once display pixels map to fractional storage
	// positions.
	EXPECT_TRUE(PressIsOnFrame(1920.f, 1080.f, 1920, 1080));
	EXPECT_TRUE(PressIsOnFrame(1919.5f, 1079.5f, 1920, 1080));
}

TEST(visual_tool_roi_press_policy, letterbox_presses_are_off_frame) {
	// Pillarbox: 16:9 storage shown in a 4:3 display area puts bars left and
	// right, so MouseToStorage returns a negative or past-the-width x.
	EXPECT_FALSE(PressIsOnFrame(-1.f, 540.f, 1920, 1080));
	EXPECT_FALSE(PressIsOnFrame(1920.5f, 540.f, 1920, 1080));
	// Letterbox: bars above and below.
	EXPECT_FALSE(PressIsOnFrame(960.f, -0.5f, 1920, 1080));
	EXPECT_FALSE(PressIsOnFrame(960.f, 1081.f, 1920, 1080));
	// A corner of the display area outside both spans.
	EXPECT_FALSE(PressIsOnFrame(-40.f, -30.f, 1920, 1080));
}

TEST(visual_tool_roi_press_policy, degenerate_frame_accepts_nothing) {
	EXPECT_FALSE(PressIsOnFrame(0.f, 0.f, 0, 1080));
	EXPECT_FALSE(PressIsOnFrame(0.f, 0.f, 1920, 0));
	EXPECT_FALSE(PressIsOnFrame(0.f, 0.f, -1920, -1080));
}

// The band branch is the destructive one: it used to commit a degenerate
// rectangle straight away, which the clamp turned into the minimum side
// glued to the frame edge, discarding the ROI the user had set up. Only an
// on-frame press may reach it.
TEST(visual_tool_roi_press_policy, only_on_frame_presses_may_start_a_band) {
	EXPECT_TRUE(PressMayStartBand(true));
	EXPECT_FALSE(PressMayStartBand(false));
}
