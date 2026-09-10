#include "subtitle_grid_projection.h"

#include "../ass_dialogue.h"
#include "../subtitle_grid_folding.h"

#include <algorithm>

namespace aegisub::presentation {
namespace {

struct RequestedGridColumns {
	bool all = false;
	bool layer = false;
	bool start = false;
	bool end = false;
	bool style = false;
	bool actor = false;
	bool effect = false;
	bool margin_left = false;
	bool margin_right = false;
	bool margin_vertical = false;
	bool cps = false;
	bool text = false;

	explicit RequestedGridColumns(std::vector<std::string> const& column_ids)
	: all(column_ids.empty()) {
		for (auto const& id : column_ids) {
			if (id == SubtitleGridColumnIdLayer)
				layer = true;
			else if (id == SubtitleGridColumnIdStart)
				start = true;
			else if (id == SubtitleGridColumnIdEnd)
				end = true;
			else if (id == SubtitleGridColumnIdStyle)
				style = true;
			else if (id == SubtitleGridColumnIdActor)
				actor = true;
			else if (id == SubtitleGridColumnIdEffect)
				effect = true;
			else if (id == SubtitleGridColumnIdMarginLeft)
				margin_left = true;
			else if (id == SubtitleGridColumnIdMarginRight)
				margin_right = true;
			else if (id == SubtitleGridColumnIdMarginVertical)
				margin_vertical = true;
			else if (id == SubtitleGridColumnIdCps)
				cps = true;
			else if (id == SubtitleGridColumnIdText)
				text = true;
		}
	}

	bool NeedsTimes() const {
		return all || start || end || cps;
	}

	bool NeedsMargins() const {
		return all || margin_left || margin_right || margin_vertical;
	}

	bool NeedsText() const {
		return all || text || cps;
	}
};

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state,
	RequestedGridColumns const& columns) {
	SubtitleGridRow row;
	row.line_id = line.Id;
	row.row_index = row_index >= 0 ? row_index : line.Row;
	row.comment = line.Comment;

	if (columns.all || columns.layer)
		row.layer = line.Layer;
	if (columns.NeedsTimes()) {
		row.start_ms = static_cast<int>(line.Start);
		row.end_ms = static_cast<int>(line.End);
	}
	if (columns.NeedsMargins())
		row.margins = line.Margin;
	if (columns.all || columns.style)
		row.style = line.Style.get();
	if (columns.all || columns.actor)
		row.actor = line.Actor.get();
	if (columns.all || columns.effect)
		row.effect = line.Effect.get();
	if (columns.NeedsText())
		row.text = line.Text.get();

	row.state = state;
	return row;
}

}

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state) {
	RequestedGridColumns columns({});
	return ProjectSubtitleGridRow(line, row_index, state, columns);
}

SubtitleGridRow ProjectSubtitleGridRow(
	AssDialogue const& line,
	int row_index,
	SubtitleGridRowState state,
	std::vector<std::string> const& column_ids) {
	RequestedGridColumns columns(column_ids);
	return ProjectSubtitleGridRow(line, row_index, state, columns);
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

	RequestedGridColumns columns(request.column_ids);
	for (int row_index = window.first_row; row_index < last_row; ++row_index) {
		auto const* line = rows[static_cast<std::size_t>(row_index)];
		if (!line)
			continue;

		auto state = resolve_state ? resolve_state(*line) : SubtitleGridRowState{};
		window.rows.push_back(ProjectSubtitleGridRow(*line, row_index, state, columns));
	}

	return window;
}

SubtitleGridWindow BuildFoldedSubtitleGridWindow(
	std::vector<SubtitleGridDisplayRow> const& rows,
	VisibleSubtitleRowsRequest const& request,
	Revision revision,
	SubtitleGridRowStateResolver const& resolve_state) {
	SubtitleGridWindow window;
	window.revision = revision;
	window.total_rows = static_cast<int>(rows.size());
	window.first_row = std::clamp(request.first_row, 0, window.total_rows);
	int const count = std::clamp(request.row_count, 0, window.total_rows - window.first_row);
	window.rows.reserve(static_cast<std::size_t>(count));

	RequestedGridColumns columns(request.column_ids);
	for (int display_row = window.first_row; display_row < window.first_row + count; ++display_row) {
		auto const& row = rows[static_cast<std::size_t>(display_row)];
		auto state = resolve_state ? resolve_state(*row.dialogue) : SubtitleGridRowState{};
		window.rows.push_back(ProjectSubtitleGridRow(*row.dialogue, row.source_row, state, columns));
	}

	return window;
}
}
