#include "subtitle_editor_ops.h"

#include "ass_dialogue.h"

#include <algorithm>

namespace aegisub::subtitle_editor_ops {

std::unique_ptr<AssDialogue> CreateLineAtVideoTime(AssDialogue const& active_line, int video_ms, int default_duration) {
	auto line = std::make_unique<AssDialogue>();
	line->Start = video_ms;
	line->End = video_ms + default_duration;
	line->Style = active_line.Style;
	return line;
}

std::unique_ptr<AssDialogue> CreateLineAfterActive(AssDialogue const& active_line, EntryList<AssDialogue>& events, int default_duration) {
	auto line = std::make_unique<AssDialogue>();
	line->Style = active_line.Style;
	line->Start = active_line.End;
	line->End = line->Start + default_duration;

	for (auto& diag : events) {
		if (diag.Start >= line->Start)
			line->End = std::min<int>(line->End, diag.Start);
	}

	return line;
}

std::unique_ptr<AssDialogue> CreateLineBeforeActive(AssDialogue const& active_line, EntryList<AssDialogue>& events, int default_duration) {
	auto line = std::make_unique<AssDialogue>();
	line->Style = active_line.Style;
	line->End = active_line.Start;
	line->Start = line->End - default_duration;

	for (auto& diag : events) {
		if (diag.End <= line->End)
			line->Start = std::max<int>(line->Start, diag.End);
	}

	return line;
}

LineSelectionResult SelectMatchingLines(EntryList<AssDialogue>& events, std::function<bool(AssDialogue const&)> const& predicate) {
	LineSelectionResult result;
	for (auto& diag : events) {
		if (!predicate(diag))
			continue;

		if (!result.active_line)
			result.active_line = &diag;
		result.ordered_lines.push_back(&diag);
	}
	return result;
}

}
