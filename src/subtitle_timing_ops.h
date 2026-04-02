#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <vector>

class AssDialogue;

namespace aegisub::subtitle_timing_ops {

struct SceneSnapFrameRange {
	int start_frame = 0;
	int one_past_end_frame = 0;
};

bool IsAdjoinableSelection(std::vector<AssDialogue *> const& sorted_selection, size_t event_count);
bool AdjoinSelection(std::vector<AssDialogue *> const& ordered_events, std::set<AssDialogue *> const& selection, bool set_start);
bool ShiftSelectionToStartTime(std::set<AssDialogue *> const& selection, AssDialogue const* active_line, int target_start);
bool SnapSelectionToVideoRange(std::set<AssDialogue *> const& selection, int start, int end, bool set_start);
std::optional<SceneSnapFrameRange> ComputeSceneSnapFrameRange(std::vector<int> const& keyframes, int current_frame, int frame_count);
bool ApplyTimeRangeToSelection(std::set<AssDialogue *> const& selection, int start_ms, int end_ms);

}
