#pragma once

#include "ass_file.h"

namespace video_subtitle_update_policy {

enum class UpdateMode {
	FullReload,
	IncrementalLines,
};

inline UpdateMode SelectUpdateMode(int commit_type, AssDialogueCommitSpan changed_lines) noexcept {
	constexpr int incremental_types = AssFile::COMMIT_DIAG_FULL;
	bool const has_dialogue_change = (commit_type & incremental_types) != 0;
	bool const has_unsupported_change = (commit_type & ~incremental_types) != 0;
	return !changed_lines.empty() && has_dialogue_change && !has_unsupported_change
		? UpdateMode::IncrementalLines
		: UpdateMode::FullReload;
}

} // namespace video_subtitle_update_policy
