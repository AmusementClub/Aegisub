// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Automation4 {
	enum class AutomationMutationRecordKind {
		Replace,
		Delete,
		DeleteRange,
		Insert,
		Append,
		UndoPoint,
		Commit,
		Apply,
		Cancel,
	};

	char const* ToString(AutomationMutationRecordKind kind) noexcept;

	struct AutomationMutationLineSnapshot {
		std::string line_class;
		std::string section;
		std::string raw;
	};

	struct AutomationMutationRecord {
		size_t sequence = 0;
		AutomationMutationRecordKind kind = AutomationMutationRecordKind::Replace;
		std::vector<int> indices;
		std::vector<AutomationMutationLineSnapshot> before_lines;
		std::vector<AutomationMutationLineSnapshot> after_lines;
		std::string description;
		int modification_type = 0;
		int line_count = 0;
		bool has_undo_description = false;
	};

	class AutomationMutationJournal {
		mutable std::mutex mutex;
		std::deque<AutomationMutationRecord> records;
		size_t max_records = 0;
		size_t total_record_count = 0;
		size_t dropped_record_count = 0;
		size_t commit_count = 0;
		size_t dropped_commit_count = 0;

		void PushRecord(AutomationMutationRecord record);

	public:
		explicit AutomationMutationJournal(size_t max_records = 10000);

		void RecordReplace(
			int index,
			AutomationMutationLineSnapshot before_line,
			AutomationMutationLineSnapshot after_line);
		void RecordDelete(
			std::vector<int> indices,
			std::vector<AutomationMutationLineSnapshot> deleted_lines);
		void RecordDeleteRange(
			int start_index,
			int end_index,
			std::vector<AutomationMutationLineSnapshot> deleted_lines);
		void RecordInsert(
			std::vector<int> inserted_indices,
			std::vector<AutomationMutationLineSnapshot> inserted_lines);
		void RecordAppend(
			std::vector<int> appended_indices,
			std::vector<AutomationMutationLineSnapshot> appended_lines);
		void RecordUndoPoint(std::string description, int modification_type, int line_count);
		void RecordCommit(std::string description, int modification_type, int line_count, bool has_undo_description);
		void RecordApply(int modification_type, int line_count);
		void RecordCancel(int pending_commit_count, int pending_delete_count, int modification_type);

		size_t RecordCount() const;
		size_t RetainedRecordCount() const;
		size_t DroppedRecordCount() const;
		size_t CommitCount() const;
		size_t DroppedCommitCount() const;
		std::vector<AutomationMutationRecord> GetRecords() const;

		bool WriteTraceFile(agi::fs::path const& path, std::string& error) const;
	};
}
