#pragma once

#include "presentation_contract.h"

#include <functional>
#include <string>
#include <vector>

class AssDialogue;
struct SubtitleGridDisplayRow;

namespace aegisub::presentation {

using SubtitleGridRowStateResolver = std::function<SubtitleGridRowState(AssDialogue const&)>;

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state = {});

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state,
	std::vector<std::string> const& column_ids);

SubtitleGridWindow BuildSubtitleGridWindow(
	std::vector<AssDialogue const*> const& rows,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver resolve_state = {});

/// Window coordinates refer to visible rows; projected row indices stay document-relative.
SubtitleGridWindow BuildFoldedSubtitleGridWindow(
	std::vector<SubtitleGridDisplayRow> const& rows,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver const& resolve_state = {});
}
