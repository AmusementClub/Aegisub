#include "video_navigation_ops.h"

#include <algorithm>

namespace aegisub::video_navigation_ops {

JumpTarget PlanNextBoundaryJump(int current_frame, int active_start_frame, int active_end_frame, std::optional<int> next_line_start_ms) {
	if (active_start_frame > current_frame)
		return {JumpTargetKind::Frame, active_start_frame, agi::vfr::START, false};

	if (active_end_frame > current_frame)
		return {JumpTargetKind::Frame, active_end_frame, agi::vfr::START, false};

	if (next_line_start_ms)
		return {JumpTargetKind::Time, *next_line_start_ms, agi::vfr::START, true};

	return {};
}

JumpTarget PlanPreviousBoundaryJump(int current_frame, int active_start_frame, int active_end_frame, std::optional<int> previous_line_end_ms) {
	if (active_end_frame < current_frame)
		return {JumpTargetKind::Frame, active_end_frame, agi::vfr::START, false};

	if (active_start_frame < current_frame)
		return {JumpTargetKind::Frame, active_start_frame, agi::vfr::START, false};

	if (previous_line_end_ms)
		return {JumpTargetKind::Time, *previous_line_end_ms, agi::vfr::END, true};

	return {};
}

int ComputeNextKeyframe(std::vector<int> const& keyframes, int current_frame, int last_frame) {
	auto pos = std::lower_bound(keyframes.begin(), keyframes.end(), current_frame + 1);
	return pos == keyframes.end() ? last_frame : *pos;
}

int ComputePreviousKeyframe(std::vector<int> const& keyframes, int current_frame) {
	if (keyframes.empty())
		return 0;

	auto pos = std::lower_bound(keyframes.begin(), keyframes.end(), current_frame);
	if (pos != keyframes.begin())
		--pos;
	return *pos;
}

}
