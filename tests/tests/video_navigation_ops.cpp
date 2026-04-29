#include <main.h>

#include "../../src/video_controller.h"
#include "../../src/video_navigation_ops.h"

#include <cstdint>

namespace {

agi::vfr::Framerate MakeNtscFilmTimecodes() {
	std::vector<int> timecodes;
	timecodes.reserve(31701);
	for (int frame = 0; frame <= 31700; ++frame)
		timecodes.push_back(static_cast<int>(static_cast<int64_t>(frame) * 1000 * 1001 / 24000));
	return agi::vfr::Framerate(timecodes);
}

int ResolveTimeJumpFrame(aegisub::video_navigation_ops::JumpTarget const& target, agi::vfr::Framerate const& fps) {
	return fps.FrameAtTime(target.value, target.time_mode);
}

}

TEST(video_navigation_ops, default_time_seek_selects_containing_frame_for_mid_frame_subtitle_start) {
	auto const fps = MakeNtscFilmTimecodes();
	int const subtitle_start_ms = 21 * 60 * 1000 + 54 * 1000 + 490;

	EXPECT_EQ(agi::vfr::EXACT, VideoController::DefaultJumpToTimeMode);
	EXPECT_EQ(31516, fps.FrameAtTime(subtitle_start_ms, VideoController::DefaultJumpToTimeMode));
	EXPECT_EQ(31517, fps.FrameAtTime(subtitle_start_ms, agi::vfr::START));
}

TEST(video_navigation_ops, explicit_line_boundary_modes_are_not_changed_by_default_exact_seek) {
	auto const fps = agi::vfr::Framerate(1.0);

	auto start = aegisub::video_navigation_ops::PlanLineBoundaryJump(1, agi::vfr::START);
	EXPECT_EQ(1, ResolveTimeJumpFrame(start, fps));
	EXPECT_EQ(0, fps.FrameAtTime(start.value, VideoController::DefaultJumpToTimeMode));

	auto end = aegisub::video_navigation_ops::PlanLineBoundaryJump(1000, agi::vfr::END);
	EXPECT_EQ(0, ResolveTimeJumpFrame(end, fps));
	EXPECT_EQ(1, fps.FrameAtTime(end.value, VideoController::DefaultJumpToTimeMode));
}

TEST(video_navigation_ops, next_boundary_prefers_active_start_then_end_then_next_line) {
	using aegisub::video_navigation_ops::JumpTargetKind;

	auto start = aegisub::video_navigation_ops::PlanNextBoundaryJump(10, 20, 40, 5000);
	EXPECT_EQ(JumpTargetKind::Frame, start.kind);
	EXPECT_EQ(20, start.value);
	EXPECT_FALSE(start.change_active_line);

	auto end = aegisub::video_navigation_ops::PlanNextBoundaryJump(20, 20, 40, 5000);
	EXPECT_EQ(JumpTargetKind::Frame, end.kind);
	EXPECT_EQ(40, end.value);
	EXPECT_FALSE(end.change_active_line);

	auto next_line = aegisub::video_navigation_ops::PlanNextBoundaryJump(40, 20, 40, 5000);
	EXPECT_EQ(JumpTargetKind::Time, next_line.kind);
	EXPECT_EQ(5000, next_line.value);
	EXPECT_EQ(agi::vfr::START, next_line.time_mode);
	EXPECT_TRUE(next_line.change_active_line);
}

TEST(video_navigation_ops, previous_boundary_prefers_active_end_then_start_then_previous_line) {
	using aegisub::video_navigation_ops::JumpTargetKind;

	auto end = aegisub::video_navigation_ops::PlanPreviousBoundaryJump(50, 20, 40, 1000);
	EXPECT_EQ(JumpTargetKind::Frame, end.kind);
	EXPECT_EQ(40, end.value);
	EXPECT_FALSE(end.change_active_line);

	auto start = aegisub::video_navigation_ops::PlanPreviousBoundaryJump(40, 20, 40, 1000);
	EXPECT_EQ(JumpTargetKind::Frame, start.kind);
	EXPECT_EQ(20, start.value);
	EXPECT_FALSE(start.change_active_line);

	auto previous_line = aegisub::video_navigation_ops::PlanPreviousBoundaryJump(20, 20, 40, 1000);
	EXPECT_EQ(JumpTargetKind::Time, previous_line.kind);
	EXPECT_EQ(1000, previous_line.value);
	EXPECT_EQ(agi::vfr::END, previous_line.time_mode);
	EXPECT_TRUE(previous_line.change_active_line);
}

TEST(video_navigation_ops, line_boundary_jump_uses_requested_time_mode_or_noop_when_missing) {
	using aegisub::video_navigation_ops::JumpTargetKind;

	auto none = aegisub::video_navigation_ops::PlanLineBoundaryJump(std::nullopt, agi::vfr::START);
	EXPECT_EQ(JumpTargetKind::None, none.kind);

	auto start = aegisub::video_navigation_ops::PlanLineBoundaryJump(1200, agi::vfr::START);
	EXPECT_EQ(JumpTargetKind::Time, start.kind);
	EXPECT_EQ(1200, start.value);
	EXPECT_EQ(agi::vfr::START, start.time_mode);
	EXPECT_FALSE(start.change_active_line);

	auto end = aegisub::video_navigation_ops::PlanLineBoundaryJump(2400, agi::vfr::END);
	EXPECT_EQ(JumpTargetKind::Time, end.kind);
	EXPECT_EQ(2400, end.value);
	EXPECT_EQ(agi::vfr::END, end.time_mode);
	EXPECT_FALSE(end.change_active_line);
}

TEST(video_navigation_ops, relative_frame_jump_applies_signed_delta) {
	EXPECT_EQ(115, aegisub::video_navigation_ops::ComputeRelativeFrameJump(100, 15));
	EXPECT_EQ(85, aegisub::video_navigation_ops::ComputeRelativeFrameJump(100, -15));
}

TEST(video_navigation_ops, next_keyframe_uses_last_frame_when_there_is_no_later_keyframe) {
	std::vector<int> keyframes = {10, 20, 30};

	EXPECT_EQ(20, aegisub::video_navigation_ops::ComputeNextKeyframe(keyframes, 10, 99));
	EXPECT_EQ(99, aegisub::video_navigation_ops::ComputeNextKeyframe(keyframes, 30, 99));
}

TEST(video_navigation_ops, previous_keyframe_matches_existing_command_behavior) {
	std::vector<int> keyframes = {10, 20, 30};

	EXPECT_EQ(0, aegisub::video_navigation_ops::ComputePreviousKeyframe({}, 50));
	EXPECT_EQ(10, aegisub::video_navigation_ops::ComputePreviousKeyframe(keyframes, 5));
	EXPECT_EQ(10, aegisub::video_navigation_ops::ComputePreviousKeyframe(keyframes, 10));
	EXPECT_EQ(20, aegisub::video_navigation_ops::ComputePreviousKeyframe(keyframes, 25));
	EXPECT_EQ(30, aegisub::video_navigation_ops::ComputePreviousKeyframe(keyframes, 99));
}
