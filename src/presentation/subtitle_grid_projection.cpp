#include "subtitle_grid_projection.h"

#include "../ass_dialogue.h"

#include <algorithm>

namespace aegisub::presentation {

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state) {
	SubtitleGridRow row;
	row.line_id = line.Id;
	row.row_index = row_index >= 0 ? row_index : line.Row;
	row.comment = line.Comment;
	row.layer = line.Layer;
	row.start_ms = static_cast<int>(line.Start);
	row.end_ms = static_cast<int>(line.End);
	row.margins = line.Margin;
	row.style = line.Style.get();
	row.actor = line.Actor.get();
	row.effect = line.Effect.get();
	row.text = line.Text.get();
	row.state = state;
	return row;
}

SubtitleGridWindow BuildSubtitleGridWindow(
	std::vector<AssDialogue const*> const& rows,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver resolve_state) {
	SubtitleGridWindow window;
	window.revision = revision;
	window.total_rows = static_cast<int>(rows.size());
	window.first_row = std::clamp(request.first_row, 0, window.total_rows);

	int const requested_count = std::max(0, request.row_count);
	int const last_row = std::min(window.total_rows, window.first_row + requested_count);
	window.rows.reserve(static_cast<std::size_t>(std::max(0, last_row - window.first_row)));

	for (int row_index = window.first_row; row_index < last_row; ++row_index) {
		auto const* line = rows[static_cast<std::size_t>(row_index)];
		if (!line)
			continue;

		auto state = resolve_state ? resolve_state(*line) : SubtitleGridRowState{};
		window.rows.push_back(ProjectSubtitleGridRow(*line, row_index, state));
	}

	return window;
}

}
