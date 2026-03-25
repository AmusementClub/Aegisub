#pragma once

#include "ass_dialogue.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegisub::subtitle_edit_ops {

enum class JoinMode {
	Karaoke,
	Concatenate,
	KeepFirst
};

struct RecombineResult {
	std::vector<AssDialogue *> lines_to_remove;
};

bool JoinSelectionIntoFirst(std::vector<AssDialogue *> const& selection, JoinMode mode);
RecombineResult RecombineSelection(std::vector<AssDialogue *> selection);
std::pair<std::string, std::string> SplitTextAtPosition(std::string const& text, int pos);
std::optional<int> EstimateSplitTime(int start_ms, int end_ms, std::string const& first_text, std::string const& second_text);
std::string BuildTagOnlyText(AssDialogue const& line);
std::string ReplaceRangeWithText(std::string text, int start, int end, std::string const& replacement);

}
