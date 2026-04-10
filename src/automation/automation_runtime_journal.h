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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation_runtime_trace_sink.h"

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Automation4 {
	struct AutomationRuntimeTraceRecord {
		size_t sequence = 0;
		AutomationRuntimeStateSnapshot snapshot;
	};

	class AutomationRuntimeJournal final : public AutomationRuntimeTraceSink {
		mutable std::mutex mutex;
		std::deque<AutomationRuntimeTraceRecord> records;
		size_t max_records = 0;
		size_t total_record_count = 0;
		size_t dropped_record_count = 0;
		size_t template_event_count = 0;
		size_t generated_line_event_count = 0;
		std::optional<std::string> last_generated_line_signature;

	public:
		explicit AutomationRuntimeJournal(size_t max_records = 10000);

		void OnRuntimeStateSnapshot(AutomationRuntimeStateSnapshot const& snapshot) override;

		size_t RecordCount() const;
		size_t RetainedRecordCount() const;
		size_t DroppedRecordCount() const;
		size_t TemplateEventCount() const;
		size_t GeneratedLineEventCount() const;
		std::optional<AutomationRuntimeStateSnapshot> LastSnapshot() const;
		std::vector<AutomationRuntimeTraceRecord> GetRecords() const;

		bool WriteTraceFile(agi::fs::path const& path, std::string& error) const;
	};
}
