#pragma once

#include "ass_file.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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

struct CoalescedUpdate {
	UpdateMode mode = UpdateMode::FullReload;
	std::vector<int> rows;
};

class UpdateCoalescer final {
public:
	void AddFullReload() {
		has_update = true;
		full_reload = true;
		rows.clear();
	}

	void AddIncrementalRows(std::span<int const> changed_rows) {
		if (changed_rows.empty()) {
			AddFullReload();
			return;
		}

		has_update = true;
		if (full_reload)
			return;
		if (std::any_of(changed_rows.begin(), changed_rows.end(), [](int row) { return row < 0; })) {
			AddFullReload();
			return;
		}

		std::vector<int> incoming(changed_rows.begin(), changed_rows.end());
		std::sort(incoming.begin(), incoming.end());
		incoming.erase(std::unique(incoming.begin(), incoming.end()), incoming.end());

		std::vector<int> merged;
		merged.reserve(rows.size() + incoming.size());
		std::set_union(
			rows.begin(), rows.end(),
			incoming.begin(), incoming.end(),
			std::back_inserter(merged));
		rows = std::move(merged);
	}

	bool Empty() const noexcept { return !has_update; }

	std::optional<CoalescedUpdate> Take() {
		if (!has_update)
			return std::nullopt;

		CoalescedUpdate update{
			full_reload ? UpdateMode::FullReload : UpdateMode::IncrementalLines,
			std::move(rows)};
		Clear();
		return update;
	}

	void Clear() noexcept {
		has_update = false;
		full_reload = false;
		rows.clear();
	}

private:
	bool has_update = false;
	bool full_reload = false;
	std::vector<int> rows;
};

} // namespace video_subtitle_update_policy
