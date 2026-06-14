#include "subtitle_grid_query_service.h"

#include "subtitle_grid_projection.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../include/aegisub/context.h"
#include "../selection_controller.h"

#include <algorithm>
#include <cstddef>

namespace aegisub::presentation {
namespace {

template<typename AssFileT, typename ResolveState>
SubtitleGridWindow QueryVisibleSubtitleRowsFromFile(
	AssFileT& file,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	ResolveState resolve_state) {
	SubtitleGridWindow window;
	window.revision = revision;

	int const first_row = std::max(0, request.first_row);
	int rows_remaining = std::max(0, request.row_count);
	window.first_row = first_row;

	if (rows_remaining > 0)
		window.rows.reserve(static_cast<std::size_t>(std::min(rows_remaining, 1024)));

	int row_index = 0;
	for (auto& line : file.Events) {
		if (row_index >= first_row && rows_remaining > 0) {
			window.rows.push_back(ProjectSubtitleGridRow(line, row_index, resolve_state(line)));
			--rows_remaining;
		}

		++row_index;
	}

	window.total_rows = row_index;
	window.first_row = std::min(first_row, window.total_rows);
	return window;
}

template<typename CoreSession>
SubtitleGridWindow QueryVisibleSubtitleRowsImpl(
	CoreSession const& core,
	VisibleSubtitleRowsRequest const& request,
	Revision revision) {
	if (!core.ass) {
		SubtitleGridWindow window;
		window.revision = revision;
		return window;
	}

	auto const* selection_controller = core.selectionController.get();
	auto const* active_line = selection_controller ? selection_controller->GetActiveLine() : nullptr;
	auto const* selected = selection_controller ? &selection_controller->GetSelectedSet() : nullptr;

	return QueryVisibleSubtitleRowsFromFile(*core.ass, request, revision, [&](AssDialogue& line) {
		auto* line_ptr = &line;
		SubtitleGridRowState state;
		state.selected = selected && selected->count(line_ptr) != 0;
		state.active = &line == active_line;
		return state;
	});
}

}

SubtitleGridWindow QueryVisibleSubtitleRows(
	AssFile const& file,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver resolve_state) {
	return QueryVisibleSubtitleRowsFromFile(file, request, revision, [&](AssDialogue const& line) {
		return resolve_state ? resolve_state(line) : SubtitleGridRowState{};
	});
}

SubtitleGridWindow QueryVisibleSubtitleRows(
	agi::ContextCoreSession const& core,
	VisibleSubtitleRowsRequest const& request,
	Revision revision) {
	return QueryVisibleSubtitleRowsImpl(core, request, revision);
}

SubtitleGridWindow QueryVisibleSubtitleRows(
	agi::ConstContextCoreSession const& core,
	VisibleSubtitleRowsRequest const& request,
	Revision revision) {
	return QueryVisibleSubtitleRowsImpl(core, request, revision);
}

}
