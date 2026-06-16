#pragma once

#include "presentation_contract.h"

#include <string>
#include <vector>

class AssDialogue;

namespace aegisub::presentation {

SubtitleGridDiff BuildSubtitleGridDiffFromCommit(
	int commit_type,
	Revision before_revision,
	Revision after_revision,
	AssDialogue const* single_line = nullptr,
	SubtitleGridRowState single_line_state = {},
	std::vector<std::string> const& column_ids = {});

}
