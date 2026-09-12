#pragma once

#include "apply_plan.h"

namespace aegisub::motion_track {

// Internal planner entry point. Frame parts provide the same coverage, gap,
// timecode and fade slicing rules to the full geometry writer.
MotionTrackApplyPlan BuildPositionApplyPlan(AssFile const& file,
											std::vector<AssDialogue *> const& targets, ApplyPlanInput const& input,
											bool frame_parts = false);

bool NeedsGeometryApply(AssDialogue const& line, TrackModel model);
MotionTrackApplyPlan BuildGeometryApplyPlan(AssFile const& file,
											AssDialogue *line, ApplyPlanInput const& input);

} // namespace aegisub::motion_track
