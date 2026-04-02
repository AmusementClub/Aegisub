#pragma once

#include "ass_file.h"

#include <functional>
#include <memory>
#include <vector>

namespace aegisub::subtitle_editor_ops {

struct LineSelectionResult {
	std::vector<AssDialogue *> ordered_lines;
	AssDialogue *active_line = nullptr;
};

std::unique_ptr<AssDialogue> CreateLineAtVideoTime(AssDialogue const& active_line, int video_ms, int default_duration);
std::unique_ptr<AssDialogue> CreateLineAfterActive(AssDialogue const& active_line, EntryList<AssDialogue>& events, int default_duration);
std::unique_ptr<AssDialogue> CreateLineBeforeActive(AssDialogue const& active_line, EntryList<AssDialogue>& events, int default_duration);
LineSelectionResult SelectMatchingLines(EntryList<AssDialogue>& events, std::function<bool(AssDialogue const&)> const& predicate);

}
