#include "subtitle_grid_diff.h"

#include "subtitle_grid_projection.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"

namespace aegisub::presentation {

SubtitleGridDiff BuildSubtitleGridDiffFromCommit(
	int commit_type,
	Revision before_revision,
	Revision after_revision,
	AssDialogue const* single_line,
	SubtitleGridRowState single_line_state,
	std::vector<std::string> const& column_ids) {
	SubtitleGridDiff diff;
	diff.before_revision = before_revision;
	diff.after_revision = after_revision;

	if (commit_type == AssFile::COMMIT_NEW
		|| (commit_type & AssFile::COMMIT_ORDER)
		|| (commit_type & AssFile::COMMIT_DIAG_ADDREM)) {
		diff.kind = SubtitleGridDiffKind::Reset;
		diff.requires_full_refresh = true;
		return diff;
	}

	if (commit_type & AssFile::COMMIT_DIAG_META) {
		diff.kind = SubtitleGridDiffKind::RowsChanged;
		diff.requires_full_refresh = true;
		return diff;
	}

	if (commit_type & AssFile::COMMIT_DIAG_TIME) {
		diff.kind = SubtitleGridDiffKind::RowsChanged;
		diff.requires_full_refresh = !single_line;
		if (single_line)
			diff.upserted_rows.push_back(ProjectSubtitleGridRow(*single_line, single_line->Row, single_line_state, column_ids));
		return diff;
	}

	if (commit_type & AssFile::COMMIT_DIAG_TEXT) {
		diff.kind = SubtitleGridDiffKind::RowsChanged;
		diff.requires_full_refresh = false;
		if (single_line)
			diff.upserted_rows.push_back(ProjectSubtitleGridRow(*single_line, single_line->Row, single_line_state, column_ids));
		return diff;
	}

	diff.kind = SubtitleGridDiffKind::Unknown;
	return diff;
}

}
