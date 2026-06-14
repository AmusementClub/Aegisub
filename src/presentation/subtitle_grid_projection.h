#pragma once

#include "presentation_contract.h"

#include <functional>
#include <vector>

class AssDialogue;

namespace aegisub::presentation {

using SubtitleGridRowStateResolver = std::function<SubtitleGridRowState(AssDialogue const&)>;

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state = {});

SubtitleGridWindow BuildSubtitleGridWindow(
	std::vector<AssDialogue const*> const& rows,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver resolve_state = {});

}
