#include "apply_source.h"

#include "../ass_file.h"

#include <algorithm>
#include <utility>

namespace aegisub::motion_track {

void MotionTrackApplySource::Capture(AssFile const& file,
									 std::vector<AssDialogue *> const& targets,
									 agi::vfr::Framerate const& timecodes, int video_frame_count) {
	Clear();
	snapshot_ = CaptureMotionTrackSource(file, targets, timecodes, video_frame_count);
	for (auto *line : targets) {
		originals_.push_back(std::make_unique<AssDialogue>(*line));
		current_lines_.push_back({line});
	}
}

void MotionTrackApplySource::Clear() {
	snapshot_ = {};
	originals_.clear();
	current_lines_.clear();
	applied_ = false;
}

std::vector<AssDialogue *> MotionTrackApplySource::Targets() const {
	std::vector<AssDialogue *> targets;
	targets.reserve(originals_.size());
	for (auto const& line : originals_)
		targets.push_back(line.get());
	return targets;
}

bool MotionTrackApplySource::ContinueTargetsMatch(
	std::vector<AssDialogue *> const& targets) const {
	// An explicit Analyze after Apply starts a new subtitle baseline. Reusing
	// the old origin with already transformed text would apply motion twice.
	return !applied_ && MotionTrackContinueTargetsMatch(snapshot_, targets);
}

bool MotionTrackApplySource::Apply(AssFile& file, MotionTrackApplyPlan const& plan,
								   agi::vfr::Framerate const& timecodes,
								   std::function<void(std::vector<AssDialogue *> const&)> const& commit,
								   std::string& message) {
	if (!MotionTrackSourceIsCurrent(file, snapshot_, timecodes)) {
		message = "The subtitles or timecodes changed; re-run Analyze first.";
		return false;
	}
	if (!plan.has_mutations()) {
		message = plan.message;
		return false;
	}

	struct Replacement {
		size_t index;
		std::vector<std::unique_ptr<AssDialogue>> parts;
	};
	std::vector<Replacement> replacements;
	for (auto const& line : plan.lines) {
		auto const original = std::ranges::find_if(originals_,
												   [&](auto const& candidate) { return candidate.get() == line.source; });
		if (original == originals_.end() || line.parts.empty()) {
			message = "The apply plan does not match the captured subtitle baseline.";
			return false;
		}
		size_t const index = original - originals_.begin();
		if (std::ranges::any_of(replacements,
								[&](auto const& item) { return item.index == index; })) {
			message = "The apply plan contains a duplicate source line.";
			return false;
		}
		Replacement replacement{.index = index, .parts = {}};
		for (auto const& part : line.parts) {
			auto output = std::make_unique<AssDialogue>(*line.source);
			output->Start = part.start_ms;
			output->End = part.end_ms;
			output->Text = part.text;
			replacement.parts.push_back(std::move(output));
		}
		replacements.push_back(std::move(replacement));
	}

	std::vector<std::unique_ptr<AssDialogue>> removed;
	std::vector<AssDialogue *> selected;
	for (auto& replacement : replacements) {
		auto& previous = current_lines_[replacement.index];
		auto anchor = file.iterator_to(*previous.front());
		std::vector<AssDialogue *> next;
		for (auto& part : replacement.parts) {
			auto *output = part.release();
			file.Events.insert(anchor, *output);
			next.push_back(output);
			selected.push_back(output);
		}
		for (auto *line : previous) {
			file.Events.erase(file.iterator_to(*line));
			removed.emplace_back(line);
		}
		previous = std::move(next);
	}
	commit(selected);

	std::vector<AssDialogue *> current;
	for (auto const& group : current_lines_)
		current.insert(current.end(), group.begin(), group.end());
	snapshot_ = CaptureMotionTrackSource(file, current, timecodes,
										 snapshot_.video_frame_count);
	applied_ = true;
	return true;
}

} // namespace aegisub::motion_track
