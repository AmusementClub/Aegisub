#pragma once

#include "apply_plan.h"
#include "../ass_dialogue.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aegisub::motion_track {

/// Keeps the Analyze-time subtitle baseline while Apply replaces its live
/// lines. Reapplying always starts from that baseline, so changing output
/// options never compounds the previously written motion or splits.
class MotionTrackApplySource {
	MotionTrackSourceSnapshot snapshot_;
	std::vector<std::unique_ptr<AssDialogue>> originals_;
	std::vector<std::vector<AssDialogue *>> current_lines_;
	bool applied_ = false;

	public:
	void Capture(AssFile const& file, std::vector<AssDialogue *> const& targets,
				 agi::vfr::Framerate const& timecodes, int video_frame_count);
	void Clear();
	[[nodiscard]] MotionTrackSourceSnapshot const& Snapshot() const { return snapshot_; }
	[[nodiscard]] std::vector<AssDialogue *> Targets() const;
	[[nodiscard]] bool ContinueTargetsMatch(std::vector<AssDialogue *> const& targets) const;

	/// Validates the current live lines again after any confirmation dialog,
	/// then replaces them with a plan built from Targets(). The callback must
	/// commit and move selection off the removed lines; those lines stay alive
	/// until it returns. The new snapshot includes commit-time row updates.
	bool Apply(AssFile& file, MotionTrackApplyPlan const& plan,
			   agi::vfr::Framerate const& timecodes,
			   std::function<void(std::vector<AssDialogue *> const&)> const& commit,
			   std::string& message);
};

} // namespace aegisub::motion_track
