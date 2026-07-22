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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation/automation_debug_session.h"
#include "automation/automation_runtime_state_snapshot.h"
#include "project_query_service.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::automation_session_service {

enum class AutomationSessionFeatureKind {
	Macro,
	ExportFilter
};

struct AutomationOutputSummary {
	std::size_t style_count = 0;
	std::size_t event_count = 0;
	std::size_t dialogue_count = 0;
	std::size_t comment_count = 0;
};

struct AutomationSessionRequest {
	agi::fs::path script_path;
	std::string feature_name;
	AutomationSessionFeatureKind feature_kind = AutomationSessionFeatureKind::Macro;
	agi::fs::path video_path;
	agi::fs::path audio_path;
	agi::fs::path subtitle_path;
	agi::fs::path output_subtitle_path;
	agi::fs::path timecodes_path;
	agi::fs::path keyframes_path;
	std::string subtitle_encoding;
	std::string output_encoding;
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	std::optional<int> video_track_index;
	std::optional<int> audio_track_index;
	std::optional<int> subtitle_track_index;
	bool skip_audio = false;
	double audio_rate_scale = 1.0;
	int audio_quantum_ms = 0;
	std::optional<agi::fs::path> trace_dir;
	std::vector<int> selected_rows;
	int active_row = 0;
	Automation4::AutomationDebugLaunchRequest debug;
	std::shared_ptr<Automation4::AutomationDebugSession> debug_session;
	bool emit_console_report = true;
};

struct AutomationSessionResult {
	int exit_code = 0;
	bool passed = false;
	bool script_loaded = false;
	bool feature_found = false;
	bool validate_ran = false;
	bool validate_passed = false;
	bool output_saved = false;
	std::size_t runtime_event_count = 0;
	std::size_t runtime_retained_event_count = 0;
	std::size_t runtime_dropped_event_count = 0;
	std::size_t template_event_count = 0;
	std::size_t generated_line_event_count = 0;
	std::size_t mutation_event_count = 0;
	std::size_t mutation_retained_event_count = 0;
	std::size_t mutation_dropped_event_count = 0;
	std::size_t mutation_commit_count = 0;
	std::size_t mutation_dropped_commit_count = 0;
	std::size_t debug_pause_count = 0;
	std::size_t debug_entry_pause_count = 0;
	std::size_t debug_breakpoint_pause_count = 0;
	std::size_t debug_step_pause_count = 0;
	std::size_t debug_dropped_pause_count = 0;
	agi::fs::path trace_dir;
	agi::fs::path runtime_trace_file;
	agi::fs::path mutation_trace_file;
	agi::fs::path debug_trace_file;
	agi::fs::path output_subtitle_path;
	project_query_service::ProjectSessionSnapshot final_project;
	AutomationOutputSummary output_summary;
	std::optional<Automation4::AutomationRuntimeStateSnapshot> final_runtime;
	std::string message;
	std::string engine_name;
	std::string script_name;
	std::string feature_name;
	std::string feature_kind;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	std::string actual_video_provider;
	std::string actual_video_decoder;
	bool video_provider_fallback = false;
	std::string video_provider_fallback_reason;
	std::string video_provider_attempts;
	std::string actual_audio_provider_factory;
	std::string actual_audio_provider;
	bool audio_provider_fallback = false;
	std::string audio_provider_fallback_reason;
	std::string audio_provider_attempts;
};

// Runs the session and invokes on_done before returning. The callback form is
// shared with headless callers, which pump their main queue while this call
// executes; GUI callers can therefore use the same service without a second
// automation implementation.
void RunAsync(AutomationSessionRequest request, std::function<void(AutomationSessionResult)> on_done);

}
