#pragma once

#include <libaegisub/vfr.h>

#include <optional>
#include <vector>

namespace aegisub::video_navigation_ops {

enum class JumpTargetKind {
	None,
	Frame,
	Time
};

struct JumpTarget {
	JumpTargetKind kind = JumpTargetKind::None;
	int value = 0;
	agi::vfr::Time time_mode = agi::vfr::START;
	bool change_active_line = false;
};

JumpTarget PlanNextBoundaryJump(int current_frame, int active_start_frame, int active_end_frame, std::optional<int> next_line_start_ms);
JumpTarget PlanPreviousBoundaryJump(int current_frame, int active_start_frame, int active_end_frame, std::optional<int> previous_line_end_ms);
int ComputeNextKeyframe(std::vector<int> const& keyframes, int current_frame, int last_frame);
int ComputePreviousKeyframe(std::vector<int> const& keyframes, int current_frame);

}
