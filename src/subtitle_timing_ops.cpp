#include "subtitle_timing_ops.h"

#include "ass_dialogue.h"

#include <algorithm>

namespace aegisub::subtitle_timing_ops {

bool IsAdjoinableSelection(std::vector<AssDialogue *> const& sorted_selection, size_t event_count) {
	if (sorted_selection.empty()) return false;
	if (sorted_selection.size() == 1 || sorted_selection.size() == event_count) return true;

	for (size_t i = 1; i < sorted_selection.size(); ++i) {
		if (sorted_selection[i]->Row != sorted_selection[i - 1]->Row + 1)
			return false;
	}
	return true;
}

bool AdjoinSelection(std::vector<AssDialogue *> const& ordered_events, std::set<AssDialogue *> const& selection, bool set_start) {
	if (selection.empty()) return false;

	AssDialogue *prev = nullptr;
	size_t seen = 0;
	bool prev_sel = false;
	for (auto *diag : ordered_events) {
		bool cur_sel = !!selection.count(diag);
		if (prev) {
			// One row selections act as if the previous or next line was selected.
			if (set_start && cur_sel && (selection.size() == 1 || prev_sel))
				diag->Start = prev->End;
			else if (!set_start && prev_sel && (cur_sel || selection.size() == 1))
				prev->End = diag->Start;
		}

		if (seen == selection.size())
			break;

		if (cur_sel)
			++seen;

		prev = diag;
		prev_sel = cur_sel;
	}

	return true;
}

bool ShiftSelectionToStartTime(std::set<AssDialogue *> const& selection, AssDialogue const* active_line, int target_start) {
	if (selection.empty() || !active_line) return false;

	int shift_by = target_start - active_line->Start;
	for (auto line : selection) {
		line->Start = line->Start + shift_by;
		line->End = line->End + shift_by;
	}
	return true;
}

bool ShiftSelectionToStartFrame(std::set<AssDialogue *> const& selection, AssDialogue const* active_line, int target_frame, agi::vfr::Framerate const& fps) {
	if (selection.empty() || !active_line) return false;

	int shift_frames = target_frame - fps.FrameAtTime(active_line->Start, agi::vfr::START);
	for (auto line : selection) {
		line->Start = fps.TimeAtFrame(shift_frames + fps.FrameAtTime(line->Start, agi::vfr::START), agi::vfr::START);
		line->End = fps.TimeAtFrame(shift_frames + fps.FrameAtTime(line->End, agi::vfr::END), agi::vfr::END);
	}
	return true;
}

bool SnapSelectionToVideoRange(std::set<AssDialogue *> const& selection, int start, int end, bool set_start) {
	if (selection.empty()) return false;

	for (auto line : selection) {
		if (set_start || line->Start > start)
			line->Start = start;
		if (!set_start || line->End < end)
			line->End = end;
	}
	return true;
}

std::optional<SceneSnapFrameRange> ComputeSceneSnapFrameRange(std::vector<int> const& keyframes, int current_frame, int frame_count) {
	if (keyframes.empty()) return std::nullopt;

	SceneSnapFrameRange range;
	if (current_frame < keyframes.front()) {
		range.one_past_end_frame = keyframes.front();
		return range;
	}

	if (current_frame >= keyframes.back()) {
		range.start_frame = keyframes.back();
		range.one_past_end_frame = frame_count;
		return range;
	}

	auto kf = std::lower_bound(keyframes.begin(), keyframes.end(), current_frame);
	if (*kf == current_frame) {
		range.start_frame = *kf;
		range.one_past_end_frame = *(kf + 1);
	}
	else {
		range.start_frame = *(kf - 1);
		range.one_past_end_frame = *kf;
	}
	return range;
}

bool ApplyTimeRangeToSelection(std::set<AssDialogue *> const& selection, int start_ms, int end_ms) {
	if (selection.empty()) return false;

	for (auto line : selection) {
		line->Start = start_ms;
		line->End = end_ms;
	}
	return true;
}

}
