#pragma once

#include "subtitle_grid_projection.h"

class AssFile;

namespace agi {
	struct ContextCoreSession;
	struct ConstContextCoreSession;
}

namespace aegisub::presentation {

SubtitleGridWindow QueryVisibleSubtitleRows(
	AssFile const& file,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver resolve_state = {});

SubtitleGridWindow QueryVisibleSubtitleRows(
	agi::ContextCoreSession const& core,
	VisibleSubtitleRowsRequest const& request,
	Revision revision);

SubtitleGridWindow QueryVisibleSubtitleRows(
	agi::ConstContextCoreSession const& core,
	VisibleSubtitleRowsRequest const& request,
	Revision revision);

}
