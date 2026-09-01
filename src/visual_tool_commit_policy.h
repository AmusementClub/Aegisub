#pragma once

#include "ass_file.h"

namespace visual_tool_commit_policy {

inline constexpr int BroadRefreshMask = AssFile::COMMIT_STYLES
	| AssFile::COMMIT_ORDER
	| AssFile::COMMIT_DIAG_ADDREM
	| AssFile::COMMIT_DIAG_META
	| AssFile::COMMIT_DIAG_TIME;
inline constexpr int LineFilteredRefreshMask = AssFile::COMMIT_DIAG_TEXT
	| AssFile::COMMIT_EXTRADATA;

struct RefreshInput {
	int commit_type = AssFile::COMMIT_NEW;
	bool local_commit = false;
	bool refresh_any_external_commit = false;
	bool has_changed_line = false;
	bool changed_line_relevant = false;
};

[[nodiscard]] inline bool UsesChangedLineFilter(int commit_type) noexcept {
	return !(commit_type & BroadRefreshMask)
		&& (commit_type & LineFilteredRefreshMask) != 0;
}

[[nodiscard]] inline bool ShouldRefreshFile(RefreshInput const& input) noexcept {
	bool const coordinate_system_changed = input.commit_type == AssFile::COMMIT_NEW
		|| (input.commit_type & AssFile::COMMIT_SCRIPTINFO) != 0;
	bool refresh = !input.local_commit
		&& !coordinate_system_changed
		&& input.refresh_any_external_commit;

	if (input.commit_type & BroadRefreshMask)
		return true;

	if (UsesChangedLineFilter(input.commit_type))
		refresh = refresh || !input.has_changed_line || input.changed_line_relevant;

	return refresh;
}

} // namespace visual_tool_commit_policy
