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

#include "automation_mutation_journal.h"

#include "automation_json_utils.h"

#include <utility>

namespace Automation4 {
namespace {
using namespace Automation4::json;

std::string SerializeIndices(std::vector<int> const& values) {
	return SerializeIntArray(values);
}

std::string SerializeLine(AutomationMutationLineSnapshot const& line) {
	JsonObjectBuilder builder;
	builder.AddRaw("line_class", JsonString(line.line_class));
	builder.AddRaw("section", JsonString(line.section));
	builder.AddRaw("raw", JsonString(line.raw));
	return builder.Build();
}

std::string SerializeLines(std::vector<AutomationMutationLineSnapshot> const& lines) {
	return SerializeObjectArray(lines, SerializeLine);
}

std::string SerializeRecord(AutomationMutationRecord const& record) {
	JsonObjectBuilder builder;
	builder.AddRaw("sequence", JsonInteger(static_cast<int>(record.sequence)));
	builder.AddRaw("kind", JsonString(ToString(record.kind)));
	builder.AddRaw("indices", SerializeIndices(record.indices));
	builder.AddRaw("before_lines", SerializeLines(record.before_lines));
	builder.AddRaw("after_lines", SerializeLines(record.after_lines));
	builder.AddRaw("description", JsonString(record.description));
	builder.AddRaw("modification_type", JsonInteger(record.modification_type));
	builder.AddRaw("line_count", JsonInteger(record.line_count));
	builder.AddRaw("has_undo_description", JsonBool(record.has_undo_description));
	return builder.Build();
}

}

char const* ToString(AutomationMutationRecordKind kind) noexcept
{
	switch (kind) {
	case AutomationMutationRecordKind::Replace: return "replace";
	case AutomationMutationRecordKind::Delete: return "delete";
	case AutomationMutationRecordKind::DeleteRange: return "delete_range";
	case AutomationMutationRecordKind::Insert: return "insert";
	case AutomationMutationRecordKind::Append: return "append";
	case AutomationMutationRecordKind::UndoPoint: return "undo_point";
	case AutomationMutationRecordKind::Commit: return "commit";
	case AutomationMutationRecordKind::Apply: return "apply";
	case AutomationMutationRecordKind::Cancel: return "cancel";
	}
	return "unknown";
}

AutomationMutationJournal::AutomationMutationJournal(size_t max_records)
: max_records(max_records) {
}

void AutomationMutationJournal::PushRecord(AutomationMutationRecord record)
{
	std::lock_guard<std::mutex> lock(mutex);
	record.sequence = ++total_record_count;
	if (record.kind == AutomationMutationRecordKind::Commit)
		++commit_count;
	records.push_back(std::move(record));

	if (!max_records)
		return;

	while (records.size() > max_records) {
		if (records.front().kind == AutomationMutationRecordKind::Commit)
			++dropped_commit_count;
		records.pop_front();
		++dropped_record_count;
	}
}

void AutomationMutationJournal::RecordReplace(
	int index,
	AutomationMutationLineSnapshot before_line,
	AutomationMutationLineSnapshot after_line)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Replace;
	record.indices = { index };
	record.before_lines.push_back(std::move(before_line));
	record.after_lines.push_back(std::move(after_line));
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordDelete(
	std::vector<int> indices,
	std::vector<AutomationMutationLineSnapshot> deleted_lines)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Delete;
	record.indices = std::move(indices);
	record.before_lines = std::move(deleted_lines);
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordDeleteRange(
	int start_index,
	int end_index,
	std::vector<AutomationMutationLineSnapshot> deleted_lines)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::DeleteRange;
	record.indices = { start_index, end_index };
	record.before_lines = std::move(deleted_lines);
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordInsert(
	std::vector<int> inserted_indices,
	std::vector<AutomationMutationLineSnapshot> inserted_lines)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Insert;
	record.indices = std::move(inserted_indices);
	record.after_lines = std::move(inserted_lines);
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordAppend(
	std::vector<int> appended_indices,
	std::vector<AutomationMutationLineSnapshot> appended_lines)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Append;
	record.indices = std::move(appended_indices);
	record.after_lines = std::move(appended_lines);
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordUndoPoint(std::string description, int modification_type, int line_count)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::UndoPoint;
	record.description = std::move(description);
	record.modification_type = modification_type;
	record.line_count = line_count;
	record.has_undo_description = true;
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordCommit(std::string description, int modification_type, int line_count, bool has_undo_description)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Commit;
	record.description = std::move(description);
	record.modification_type = modification_type;
	record.line_count = line_count;
	record.has_undo_description = has_undo_description;
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordApply(int modification_type, int line_count)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Apply;
	record.modification_type = modification_type;
	record.line_count = line_count;
	PushRecord(std::move(record));
}

void AutomationMutationJournal::RecordCancel(int pending_commit_count, int pending_delete_count, int modification_type)
{
	AutomationMutationRecord record;
	record.kind = AutomationMutationRecordKind::Cancel;
	record.indices = { pending_commit_count, pending_delete_count };
	record.modification_type = modification_type;
	PushRecord(std::move(record));
}

size_t AutomationMutationJournal::RecordCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return total_record_count;
}

size_t AutomationMutationJournal::RetainedRecordCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return records.size();
}

size_t AutomationMutationJournal::DroppedRecordCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return dropped_record_count;
}

size_t AutomationMutationJournal::CommitCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return commit_count;
}

size_t AutomationMutationJournal::DroppedCommitCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return dropped_commit_count;
}

std::vector<AutomationMutationRecord> AutomationMutationJournal::GetRecords() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return { records.begin(), records.end() };
}

bool AutomationMutationJournal::WriteTraceFile(agi::fs::path const& path, std::string& error) const
{
	std::vector<AutomationMutationRecord> snapshot_records;
	{
		std::lock_guard<std::mutex> lock(mutex);
		snapshot_records.assign(records.begin(), records.end());
	}

	return WriteJsonLinesFile(
		path,
		snapshot_records,
		SerializeRecord,
		"could not open automation mutation trace file for writing",
		error);
}

}
