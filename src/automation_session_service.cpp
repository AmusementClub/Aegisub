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

#include "automation_session_service.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "auto4_base.h"
#include "command/command.h"
#include "compat.h"
#include "headless_playback_session_host.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "provider_selection_diagnostics.h"
#include "selection_controller.h"
#include "subtitle_format.h"
#include "subs_controller.h"
#include "ui_services.h"

#include "automation/automation_live_host.h"
#include "automation/automation_debug_session.h"
#include "automation/automation_mutation_journal.h"
#include "automation/automation_runtime_journal.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace aegisub::automation_session_service {
namespace {

using headless_playback_session_host::BoolString;
using headless_playback_session_host::DescribeProviderFallback;
using headless_playback_session_host::FormatProviderAttempts;
using headless_playback_session_host::PlaybackSessionHost;
using headless_playback_session_host::PlaybackSessionHostOptions;
using headless_playback_session_host::UsedProviderFallback;
using project_query_service::ProjectSessionSnapshot;

constexpr char kAutomationRuntimeTraceFileName[] = "automation-runtime.ndjson";
constexpr char kAutomationMutationTraceFileName[] = "automation-mutations.ndjson";
constexpr char kAutomationDebugTraceFileName[] = "automation-debug.ndjson";

std::string Sanitize(std::string const& value) {
	return provider_selection_diagnostics::SanitizeText(value);
}

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

std::string FeatureKindName(AutomationSessionFeatureKind kind) {
	switch (kind) {
	case AutomationSessionFeatureKind::Macro:
		return "macro";
	case AutomationSessionFeatureKind::ExportFilter:
		return "export_filter";
	}
	return "unknown";
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

bool MatchesFeatureName(std::string const& requested, std::string const& candidate) {
	return requested == candidate || Lower(requested) == Lower(candidate);
}

std::string JoinRows(std::vector<int> const& rows) {
	std::ostringstream out;
	for (size_t i = 0; i < rows.size(); ++i) {
		if (i != 0)
			out << ",";
		out << rows[i];
	}
	return out.str();
}

AutomationOutputSummary SummarizeAssFile(AssFile const& ass) {
	AutomationOutputSummary summary;
	summary.style_count = ass.Styles.size();
	for (auto const& line : ass.Events) {
		++summary.event_count;
		if (line.Comment)
			++summary.comment_count;
		else
			++summary.dialogue_count;
	}
	return summary;
}

void WriteProjectSnapshot(std::ofstream& out, std::string const& prefix, ProjectSessionSnapshot const& snapshot) {
	auto write_value = [&](std::string const& key, std::string const& value) {
		out << key << "=" << Sanitize(value) << "\n";
	};
	auto write_bool = [&](std::string const& key, bool value) {
		out << key << "=" << BoolString(value) << "\n";
	};

	write_value(prefix + ".subtitle_path", ToGenericString(snapshot.subtitle_path));
	write_bool(prefix + ".subtitle_file_loaded", snapshot.subtitle_file_loaded);
	write_bool(prefix + ".subtitle_modified", snapshot.subtitle_modified);
	out << prefix << ".style_count=" << snapshot.style_count << "\n";
	out << prefix << ".event_count=" << snapshot.event_count << "\n";
	out << prefix << ".dialogue_count=" << snapshot.dialogue_count << "\n";
	out << prefix << ".comment_count=" << snapshot.comment_count << "\n";
	write_value(prefix + ".title", snapshot.title);
	write_value(prefix + ".video_path", ToGenericString(snapshot.media.video_path));
	write_value(prefix + ".audio_path", ToGenericString(snapshot.media.audio_path));
	write_bool(prefix + ".has_video", snapshot.media.has_video);
	write_bool(prefix + ".has_audio", snapshot.media.has_audio);
	write_bool(prefix + ".can_load_subtitles_from_video", snapshot.media.can_load_subtitles_from_video);
	out << prefix << ".video_width=" << snapshot.media.video_width << "\n";
	out << prefix << ".video_height=" << snapshot.media.video_height << "\n";
	out << prefix << ".video_frame_count=" << snapshot.media.video_frame_count << "\n";
	out << prefix << ".video_duration_ms=" << snapshot.media.video_duration_ms << "\n";
	write_value(prefix + ".video_decoder_name", snapshot.media.video_decoder_name);
	out << prefix << ".audio_sample_rate=" << snapshot.media.audio_sample_rate << "\n";
	out << prefix << ".audio_num_samples=" << snapshot.media.audio_num_samples << "\n";
	out << prefix << ".audio_duration_ms=" << snapshot.media.audio_duration_ms << "\n";
	write_value(prefix + ".audio_provider_name", snapshot.media.audio_provider_name);
	write_value(prefix + ".timecodes_path", ToGenericString(snapshot.timecodes_path));
	write_bool(prefix + ".timecodes_file_loaded", snapshot.timecodes_file_loaded);
	write_bool(prefix + ".timecodes_loaded", snapshot.timecodes_loaded);
	write_value(prefix + ".keyframes_path", ToGenericString(snapshot.keyframes_path));
	write_bool(prefix + ".keyframes_file_loaded", snapshot.keyframes_file_loaded);
	write_bool(prefix + ".keyframes_loaded", snapshot.keyframes_loaded);
	out << prefix << ".keyframe_count=" << snapshot.keyframe_count << "\n";
}

void PrintProjectSnapshot(ProjectSessionSnapshot const& snapshot, std::string const& prefix) {
	std::cout << prefix << ".subtitle_path=" << ToGenericString(snapshot.subtitle_path) << "\n";
	std::cout << prefix << ".subtitle_file_loaded=" << BoolString(snapshot.subtitle_file_loaded) << "\n";
	std::cout << prefix << ".subtitle_modified=" << BoolString(snapshot.subtitle_modified) << "\n";
	std::cout << prefix << ".style_count=" << snapshot.style_count << "\n";
	std::cout << prefix << ".event_count=" << snapshot.event_count << "\n";
	std::cout << prefix << ".dialogue_count=" << snapshot.dialogue_count << "\n";
	std::cout << prefix << ".comment_count=" << snapshot.comment_count << "\n";
	std::cout << prefix << ".video_path=" << ToGenericString(snapshot.media.video_path) << "\n";
	std::cout << prefix << ".audio_path=" << ToGenericString(snapshot.media.audio_path) << "\n";
	std::cout << prefix << ".has_video=" << BoolString(snapshot.media.has_video) << "\n";
	std::cout << prefix << ".has_audio=" << BoolString(snapshot.media.has_audio) << "\n";
	std::cout << prefix << ".timecodes_path=" << ToGenericString(snapshot.timecodes_path) << "\n";
	std::cout << prefix << ".timecodes_file_loaded=" << BoolString(snapshot.timecodes_file_loaded) << "\n";
	std::cout << prefix << ".keyframes_path=" << ToGenericString(snapshot.keyframes_path) << "\n";
	std::cout << prefix << ".keyframes_file_loaded=" << BoolString(snapshot.keyframes_file_loaded) << "\n";
}

void WriteFinalRuntimeSummary(std::ofstream& out, Automation4::AutomationRuntimeStateSnapshot const& snapshot) {
	out << "automation.final_runtime.invocation.kind=" << Automation4::ToString(snapshot.invocation.kind) << "\n";
	out << "automation.final_runtime.invocation.feature_name=" << Sanitize(snapshot.invocation.feature_name) << "\n";
	out << "automation.final_runtime.selection.selected_rows=" << Sanitize(JoinRows(snapshot.context_snapshot.selection.selected_rows)) << "\n";
	out << "automation.final_runtime.selection.active_row=" << snapshot.context_snapshot.selection.active_row << "\n";
	out << "automation.final_runtime.media.has_video=" << BoolString(snapshot.context_snapshot.media.has_video) << "\n";
	out << "automation.final_runtime.media.has_keyframes=" << BoolString(snapshot.context_snapshot.media.has_keyframes) << "\n";
	out << "automation.final_runtime.project.active_row=" << snapshot.context_snapshot.project.active_row << "\n";
	if (snapshot.template_debug) {
		if (snapshot.template_debug->kind)
			out << "automation.final_runtime.template.kind=" << Sanitize(*snapshot.template_debug->kind) << "\n";
		if (snapshot.template_debug->phase)
			out << "automation.final_runtime.template.phase=" << Sanitize(*snapshot.template_debug->phase) << "\n";
		if (snapshot.template_debug->scope_kind)
			out << "automation.final_runtime.template.scope_kind=" << Sanitize(*snapshot.template_debug->scope_kind) << "\n";
		if (snapshot.template_debug->identity && snapshot.template_debug->identity->template_debug_id)
			out << "automation.final_runtime.template.template_debug_id=" << *snapshot.template_debug->identity->template_debug_id << "\n";
		if (snapshot.template_debug->generated && snapshot.template_debug->generated->count)
			out << "automation.final_runtime.template.generated_count=" << *snapshot.template_debug->generated->count << "\n";
	}
}

class HeadlessAutomationBackgroundScriptRunnerFactory final : public Automation4::AutomationBackgroundScriptRunnerFactory {
public:
	std::unique_ptr<Automation4::BackgroundScriptRunner> Create(std::string const& title) override {
		return std::make_unique<Automation4::BackgroundScriptRunner>(
			std::make_unique<agi::detail::InlineBackgroundRunner>(),
			nullptr,
			title);
	}
};

AssDialogue *ResolveDialogueByAutomationRow(agi::ContextCoreSession const& core, int automation_row) {
	if (automation_row <= 0)
		return nullptr;

	int const offset = static_cast<int>(core.ass->Info.size() + core.ass->Styles.size()) + 1;
	for (auto& dialogue : core.ass->Events) {
		if (dialogue.Row + offset == automation_row)
			return &dialogue;
	}
	return nullptr;
}

bool ConfigureSelection(
	agi::ContextCoreSession const& core,
	std::vector<int> const& requested_rows,
	int requested_active_row,
	std::string& error) {
	Selection selection;
	AssDialogue *active_line = nullptr;

	for (int row : requested_rows) {
		auto *dialogue = ResolveDialogueByAutomationRow(core, row);
		if (!dialogue) {
			error = "requested automation selection row is out of bounds: " + std::to_string(row);
			return false;
		}
		selection.insert(dialogue);
	}

	if (requested_active_row > 0) {
		active_line = ResolveDialogueByAutomationRow(core, requested_active_row);
		if (!active_line) {
			error = "requested automation active row is out of bounds: " + std::to_string(requested_active_row);
			return false;
		}
	}

	if (!active_line && !selection.empty())
		active_line = *selection.begin();

	if (selection.empty() && active_line)
		selection.insert(active_line);

	if (selection.empty() && !core.ass->Events.empty()) {
		active_line = &core.ass->Events.front();
		selection.insert(active_line);
	}

	core.selectionController->SetSelectionAndActive(std::move(selection), active_line);
	return true;
}

Automation4::Script *EnsureScriptLoaded(
	agi::ContextCoreSession const& core,
	agi::fs::path const& script_path,
	std::string *error) {
	for (auto const& existing : core.local_scripts->GetScripts()) {
		if (existing && existing->GetFilename() == script_path)
			return existing.get();
	}

	bool recognised = false;
	auto script = Automation4::ScriptFactory::CreateFromFile(script_path, false, &recognised);
	if (!script) {
		if (error) {
			if (recognised)
				*error = "could not load automation script: " + ToGenericString(script_path);
			else
				*error = "automation script file was not recognised as an Automation script: " + ToGenericString(script_path);
		}
		return nullptr;
	}

	auto *raw = script.get();
	core.local_scripts->Add(std::move(script));
	return raw;
}

cmd::Command *FindMacro(Automation4::Script const& script, std::string const& requested_name) {
	for (auto *macro : script.GetMacros()) {
		if (!macro)
			continue;

		auto const internal_name = std::string(macro->name());
		auto const display_name = from_wx(macro->StrDisplay(nullptr));
		if (MatchesFeatureName(requested_name, internal_name) || MatchesFeatureName(requested_name, display_name))
			return macro;
	}
	return nullptr;
}

Automation4::ExportFilter *FindFilter(Automation4::Script const& script, std::string const& requested_name) {
	for (auto *filter : script.GetFilters()) {
		if (filter && MatchesFeatureName(requested_name, filter->GetName()))
			return filter;
	}
	return nullptr;
}

bool SaveSubtitleFile(
	AssFile const& subs,
	agi::fs::path const& output_path,
	agi::vfr::Framerate const& fps,
	std::string const& encoding,
	std::shared_ptr<agi::SingleChoiceInteractionSink> const& choice_sink,
	std::string& error) {
	if (!output_path.parent_path().empty())
		agi::fs::CreateDirectory(output_path.parent_path());

	auto const *writer = SubtitleFormat::GetWriter(output_path);
	if (!writer) {
		error = "could not find subtitle writer for output path: " + ToGenericString(output_path);
		return false;
	}

	try {
		writer->ExportFile(&subs, output_path, fps, encoding, choice_sink);
		return true;
	}
	catch (agi::Exception const& e) {
		error = e.GetMessage();
		return false;
	}
	catch (std::exception const& e) {
		error = e.what();
		return false;
	}
}

class Runner final {
	AutomationSessionRequest request;
	std::function<void(AutomationSessionResult)> on_done;
	PlaybackSessionHost runtime;
	Automation4::AutomationRuntimeJournal journal;
	std::shared_ptr<Automation4::AutomationMutationJournal> mutation_journal = std::make_shared<Automation4::AutomationMutationJournal>();
	std::shared_ptr<Automation4::AutomationDebugSession> debug_session;
	std::shared_ptr<HeadlessAutomationBackgroundScriptRunnerFactory> automation_runner_factory = std::make_shared<HeadlessAutomationBackgroundScriptRunnerFactory>();
	bool finished = false;
	bool host_started = false;
	bool script_loaded = false;
	bool feature_found = false;
	bool validate_ran = false;
	bool validate_passed = false;
	bool output_saved = false;
	AutomationOutputSummary output_summary;
	std::string engine_name;
	std::string script_name;
	std::string resolved_feature_name;

	void AppendHostLog(std::string const& message) const {
		if (runtime.TraceDir().empty())
			return;

		auto out = agi::io::OpenOutputFileStream(runtime.TraceDir() / "automation-session-host.log", std::ios::out | std::ios::app);
		if (!out)
			return;
		out << message << "\n";
	}

	bool OpenInitialProjectState() {
		AppendHostLog("open-state.begin");
		auto core = runtime.GetCore();
		core.automationBackgroundScriptRunnerFactory = automation_runner_factory;

		auto const has_explicit_media = !request.video_path.empty() || !request.audio_path.empty();
		if (!request.subtitle_path.empty()) {
			AppendHostLog("open-state.load-subtitles.begin");
			core.project->LoadSubtitles(request.subtitle_path, request.subtitle_encoding, !has_explicit_media);
			AppendHostLog("open-state.load-subtitles.end");
		}

		if (has_explicit_media) {
			AppendHostLog("open-state.open-media.begin");
			project_open_service::PlaybackOpenOptions options;
			options.video_path = request.video_path;
			options.skip_audio = request.skip_audio;
			if (!request.skip_audio) {
				if (!request.audio_path.empty())
					options.audio_path = request.audio_path;
				else if (!request.video_path.empty())
					options.audio_path = request.video_path;
			}

			auto result = runtime.OpenMedia(options);
			if (!result.opened) {
				Finish(40, result.error.empty() ? "automation session failed to open media" : result.error);
				return false;
			}
			AppendHostLog("open-state.open-media.end");
		}

		if (!request.timecodes_path.empty()) {
			AppendHostLog("open-state.load-timecodes.begin");
			core.project->LoadTimecodes(request.timecodes_path);
			AppendHostLog("open-state.load-timecodes.end");
		}
		if (!request.keyframes_path.empty()) {
			AppendHostLog("open-state.load-keyframes.begin");
			core.project->LoadKeyframes(request.keyframes_path);
			AppendHostLog("open-state.load-keyframes.end");
		}

		std::string selection_error;
		AppendHostLog("open-state.selection.begin");
		if (!ConfigureSelection(core, request.selected_rows, request.active_row, selection_error)) {
			Finish(40, selection_error);
			return false;
		}
		AppendHostLog("open-state.selection.end");
		AppendHostLog("open-state.end");

		return true;
	}

	AutomationSessionResult BuildResult(int exit_code, std::string message) {
		AutomationSessionResult result;
		result.exit_code = exit_code;
		result.passed = exit_code == 0;
		result.script_loaded = script_loaded;
		result.feature_found = feature_found;
		result.validate_ran = validate_ran;
		result.validate_passed = validate_passed;
		result.output_saved = output_saved;
		result.runtime_event_count = journal.RecordCount();
		result.runtime_retained_event_count = journal.RetainedRecordCount();
		result.runtime_dropped_event_count = journal.DroppedRecordCount();
		result.template_event_count = journal.TemplateEventCount();
		result.generated_line_event_count = journal.GeneratedLineEventCount();
		result.mutation_event_count = mutation_journal->RecordCount();
		result.mutation_retained_event_count = mutation_journal->RetainedRecordCount();
		result.mutation_dropped_event_count = mutation_journal->DroppedRecordCount();
		result.mutation_commit_count = mutation_journal->CommitCount();
		result.mutation_dropped_commit_count = mutation_journal->DroppedCommitCount();
		result.trace_dir = runtime.TraceDir();
		result.runtime_trace_file = runtime.TraceDir().empty()
			? agi::fs::path()
			: runtime.TraceDir() / kAutomationRuntimeTraceFileName;
		result.mutation_trace_file = runtime.TraceDir().empty()
			? agi::fs::path()
			: runtime.TraceDir() / kAutomationMutationTraceFileName;
		result.debug_trace_file = (debug_session && !runtime.TraceDir().empty())
			? runtime.TraceDir() / kAutomationDebugTraceFileName
			: agi::fs::path();
		result.output_subtitle_path = request.output_subtitle_path;
		result.output_summary = output_summary;
		result.final_runtime = journal.LastSnapshot();
		result.message = std::move(message);
		result.engine_name = engine_name;
		result.script_name = script_name;
		result.feature_name = resolved_feature_name.empty() ? request.feature_name : resolved_feature_name;
		result.feature_kind = FeatureKindName(request.feature_kind);
		result.selected_video_provider = runtime.SelectedVideoProvider();
		result.selected_audio_provider = runtime.SelectedAudioProvider();
		result.actual_video_provider = runtime.ActualVideoProvider();
		result.actual_video_decoder = runtime.ActualVideoDecoder();
		result.video_provider_fallback = UsedProviderFallback(runtime.VideoProviderReport());
		result.video_provider_fallback_reason = DescribeProviderFallback(runtime.VideoProviderReport());
		result.video_provider_attempts = FormatProviderAttempts(runtime.VideoProviderReport());
		result.actual_audio_provider_factory = runtime.ActualAudioProviderFactory();
		result.actual_audio_provider = runtime.ActualAudioProvider();
		result.audio_provider_fallback = UsedProviderFallback(runtime.AudioProviderReport());
		result.audio_provider_fallback_reason = DescribeProviderFallback(runtime.AudioProviderReport());
		result.audio_provider_attempts = FormatProviderAttempts(runtime.AudioProviderReport());
		if (debug_session) {
			result.debug_pause_count = debug_session->PauseCount();
			result.debug_entry_pause_count = debug_session->EntryPauseCount();
			result.debug_breakpoint_pause_count = debug_session->BreakpointPauseCount();
			result.debug_step_pause_count = debug_session->StepPauseCount();
			result.debug_dropped_pause_count = debug_session->DroppedPauseCount();
		}
		if (host_started)
			result.final_project = project_query_service::QueryProjectSession(runtime.GetCore());
		return result;
	}

	void WriteManifest(AutomationSessionResult const& result) const {
		if (runtime.TraceDir().empty())
			return;

		auto out = agi::io::OpenOutputFileStream(runtime.TraceDir() / "manifest.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		out << "command=session automation\n";
		out << "session.script=" << Sanitize(ToGenericString(request.script_path)) << "\n";
		out << "session.feature.kind=" << Sanitize(FeatureKindName(request.feature_kind)) << "\n";
		out << "session.feature.name=" << Sanitize(result.feature_name) << "\n";
		out << "session.subtitle=" << Sanitize(ToGenericString(request.subtitle_path)) << "\n";
		out << "session.output_subtitle=" << Sanitize(ToGenericString(request.output_subtitle_path)) << "\n";
		out << "session.timecodes=" << Sanitize(ToGenericString(request.timecodes_path)) << "\n";
		out << "session.keyframes=" << Sanitize(ToGenericString(request.keyframes_path)) << "\n";
		out << "session.video=" << Sanitize(ToGenericString(request.video_path)) << "\n";
		out << "session.audio=" << Sanitize(ToGenericString(request.audio_path)) << "\n";
		out << "session.skip_audio=" << BoolString(request.skip_audio) << "\n";
		out << "session.selected_rows=" << Sanitize(JoinRows(request.selected_rows)) << "\n";
		out << "session.active_row=" << request.active_row << "\n";
		out << "session.debug.enabled=" << BoolString(request.debug.enabled) << "\n";
		out << "session.debug.stop_on_entry=" << BoolString(request.debug.stop_on_entry) << "\n";
		out << "session.debug.auto_step_count=" << request.debug.auto_step_count << "\n";
		out << "session.runtime_trace_file=" << Sanitize(ToGenericString(result.runtime_trace_file)) << "\n";
		out << "session.mutation_trace_file=" << Sanitize(ToGenericString(result.mutation_trace_file)) << "\n";
		out << "session.debug_trace_file=" << Sanitize(ToGenericString(result.debug_trace_file)) << "\n";
		out << "session.host_log_file=" << Sanitize(ToGenericString(runtime.TraceDir() / "automation-session-host.log")) << "\n";
	}

	void WriteSummary(AutomationSessionResult const& result) const {
		if (runtime.TraceDir().empty())
			return;

		auto out = agi::io::OpenOutputFileStream(runtime.TraceDir() / "summary.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		auto write_value = [&](std::string const& key, std::string const& value) {
			out << key << "=" << Sanitize(value) << "\n";
		};
		auto write_bool = [&](std::string const& key, bool value) {
			out << key << "=" << BoolString(value) << "\n";
		};

		write_value("automation.engine", result.engine_name);
		write_value("automation.script_name", result.script_name);
		write_bool("automation.script_loaded", result.script_loaded);
		write_value("automation.feature.kind", result.feature_kind);
		write_value("automation.feature.name", result.feature_name);
		write_bool("automation.feature_found", result.feature_found);
		write_bool("automation.validate.ran", result.validate_ran);
		write_bool("automation.validate.passed", result.validate_passed);
		out << "automation.trace.event_count=" << result.runtime_event_count << "\n";
		out << "automation.trace.retained_event_count=" << result.runtime_retained_event_count << "\n";
		out << "automation.trace.dropped_event_count=" << result.runtime_dropped_event_count << "\n";
		out << "automation.trace.template_event_count=" << result.template_event_count << "\n";
		out << "automation.trace.generated_line_event_count=" << result.generated_line_event_count << "\n";
		write_value("automation.trace.file", ToGenericString(result.runtime_trace_file));
		out << "automation.mutation.event_count=" << result.mutation_event_count << "\n";
		out << "automation.mutation.retained_event_count=" << result.mutation_retained_event_count << "\n";
		out << "automation.mutation.dropped_event_count=" << result.mutation_dropped_event_count << "\n";
		out << "automation.mutation.commit_count=" << result.mutation_commit_count << "\n";
		out << "automation.mutation.dropped_commit_count=" << result.mutation_dropped_commit_count << "\n";
		write_value("automation.mutation.file", ToGenericString(result.mutation_trace_file));
		out << "automation.debug.pause_count=" << result.debug_pause_count << "\n";
		out << "automation.debug.entry_pause_count=" << result.debug_entry_pause_count << "\n";
		out << "automation.debug.breakpoint_pause_count=" << result.debug_breakpoint_pause_count << "\n";
		out << "automation.debug.step_pause_count=" << result.debug_step_pause_count << "\n";
		out << "automation.debug.dropped_pause_count=" << result.debug_dropped_pause_count << "\n";
		write_value("automation.debug.file", ToGenericString(result.debug_trace_file));
		write_bool("automation.output.saved", result.output_saved);
		write_value("automation.output.path", ToGenericString(result.output_subtitle_path));
		out << "automation.output.style_count=" << result.output_summary.style_count << "\n";
		out << "automation.output.event_count=" << result.output_summary.event_count << "\n";
		out << "automation.output.dialogue_count=" << result.output_summary.dialogue_count << "\n";
		out << "automation.output.comment_count=" << result.output_summary.comment_count << "\n";
		write_value("automation.selected.video_provider", result.selected_video_provider);
		write_value("automation.selected.audio_provider", result.selected_audio_provider);
		write_value("automation.actual.video_provider", result.actual_video_provider);
		write_value("automation.actual.video_decoder", result.actual_video_decoder);
		write_bool("automation.video.provider_fallback", result.video_provider_fallback);
		write_value("automation.video.provider_fallback_reason", result.video_provider_fallback_reason);
		write_value("automation.video.provider_attempts", result.video_provider_attempts);
		write_value("automation.actual.audio_provider_factory", result.actual_audio_provider_factory);
		write_value("automation.actual.audio_provider", result.actual_audio_provider);
		write_bool("automation.audio.provider_fallback", result.audio_provider_fallback);
		write_value("automation.audio.provider_fallback_reason", result.audio_provider_fallback_reason);
		write_value("automation.audio.provider_attempts", result.audio_provider_attempts);
		WriteProjectSnapshot(out, "automation.project.final", result.final_project);
		if (result.final_runtime)
			WriteFinalRuntimeSummary(out, *result.final_runtime);
		out << "automation.result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		write_value("automation.message", result.message);
	}

	void PrintReport(AutomationSessionResult const& result) const {
		std::cout << "headless-automation-session\n";
		std::cout << "script=" << ToGenericString(request.script_path) << "\n";
		std::cout << "feature.kind=" << result.feature_kind << "\n";
		std::cout << "feature.name=" << result.feature_name << "\n";
		std::cout << "subtitle=" << ToGenericString(request.subtitle_path) << "\n";
		std::cout << "output_subtitle=" << ToGenericString(result.output_subtitle_path) << "\n";
		std::cout << "timecodes=" << ToGenericString(request.timecodes_path) << "\n";
		std::cout << "keyframes=" << ToGenericString(request.keyframes_path) << "\n";
		std::cout << "video=" << ToGenericString(request.video_path) << "\n";
		std::cout << "audio=" << ToGenericString(request.audio_path) << "\n";
		std::cout << "skip_audio=" << BoolString(request.skip_audio) << "\n";
		std::cout << "script_loaded=" << BoolString(result.script_loaded) << "\n";
		std::cout << "feature_found=" << BoolString(result.feature_found) << "\n";
		std::cout << "validate.ran=" << BoolString(result.validate_ran) << "\n";
		std::cout << "validate.passed=" << BoolString(result.validate_passed) << "\n";
		std::cout << "trace.event_count=" << result.runtime_event_count << "\n";
		std::cout << "trace.retained_event_count=" << result.runtime_retained_event_count << "\n";
		std::cout << "trace.dropped_event_count=" << result.runtime_dropped_event_count << "\n";
		std::cout << "trace.template_event_count=" << result.template_event_count << "\n";
		std::cout << "trace.generated_line_event_count=" << result.generated_line_event_count << "\n";
		std::cout << "mutation.event_count=" << result.mutation_event_count << "\n";
		std::cout << "mutation.retained_event_count=" << result.mutation_retained_event_count << "\n";
		std::cout << "mutation.dropped_event_count=" << result.mutation_dropped_event_count << "\n";
		std::cout << "mutation.commit_count=" << result.mutation_commit_count << "\n";
		std::cout << "mutation.dropped_commit_count=" << result.mutation_dropped_commit_count << "\n";
		std::cout << "debug.pause_count=" << result.debug_pause_count << "\n";
		std::cout << "debug.entry_pause_count=" << result.debug_entry_pause_count << "\n";
		std::cout << "debug.breakpoint_pause_count=" << result.debug_breakpoint_pause_count << "\n";
		std::cout << "debug.step_pause_count=" << result.debug_step_pause_count << "\n";
		std::cout << "debug.dropped_pause_count=" << result.debug_dropped_pause_count << "\n";
		std::cout << "output_saved=" << BoolString(result.output_saved) << "\n";
		std::cout << "output.style_count=" << result.output_summary.style_count << "\n";
		std::cout << "output.event_count=" << result.output_summary.event_count << "\n";
		std::cout << "output.dialogue_count=" << result.output_summary.dialogue_count << "\n";
		std::cout << "output.comment_count=" << result.output_summary.comment_count << "\n";
		PrintProjectSnapshot(result.final_project, "final");
		std::cout << "trace_dir=" << ToGenericString(result.trace_dir) << "\n";
		std::cout << "runtime_trace_file=" << ToGenericString(result.runtime_trace_file) << "\n";
		std::cout << "mutation_trace_file=" << ToGenericString(result.mutation_trace_file) << "\n";
		std::cout << "debug_trace_file=" << ToGenericString(result.debug_trace_file) << "\n";
		std::cout << "result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		if (!result.message.empty())
			std::cout << "message=" << result.message << "\n";
	}

	void Finish(int exit_code, std::string message) {
		if (finished)
			return;
		finished = true;
		AppendHostLog("finish.begin");

		if (host_started) {
			for (auto const& script : runtime.GetCore().local_scripts->GetScripts()) {
				if (script) {
					script->SetRuntimeTraceSink(nullptr);
					script->SetAutomationHost(nullptr);
					script->SetDebugSession(nullptr);
				}
			}
		}

		auto result = BuildResult(exit_code, std::move(message));
		std::string trace_error;
		if (!result.runtime_trace_file.empty() && !journal.WriteTraceFile(result.runtime_trace_file, trace_error)) {
			result.exit_code = result.exit_code == 0 ? 49 : result.exit_code;
			result.passed = false;
			if (result.message.empty())
				result.message = trace_error;
			else
				result.message += " | " + trace_error;
		}
		std::string mutation_trace_error;
		if (!result.mutation_trace_file.empty() && !mutation_journal->WriteTraceFile(result.mutation_trace_file, mutation_trace_error)) {
			result.exit_code = result.exit_code == 0 ? 49 : result.exit_code;
			result.passed = false;
			if (result.message.empty())
				result.message = mutation_trace_error;
			else
				result.message += " | " + mutation_trace_error;
		}
		std::string debug_trace_error;
		if (debug_session && !result.debug_trace_file.empty() && !debug_session->WriteTraceFile(result.debug_trace_file, debug_trace_error)) {
			result.exit_code = result.exit_code == 0 ? 49 : result.exit_code;
			result.passed = false;
			if (result.message.empty())
				result.message = debug_trace_error;
			else
				result.message += " | " + debug_trace_error;
		}
		if (debug_session)
			debug_session->MarkCompleted(result.exit_code, result.message);

		runtime.ShutdownTrace();
		WriteManifest(result);
		WriteSummary(result);
		if (request.emit_console_report)
			PrintReport(result);
		runtime.CloseMedia();
		runtime.ReleaseResources();
		AppendHostLog("finish.end");

		if (on_done)
			on_done(std::move(result));
	}

	void ExecuteMacro(Automation4::Script& script) {
		auto *macro = FindMacro(script, request.feature_name);
		if (!macro) {
			Finish(41, "automation macro not found in script: " + request.feature_name);
			return;
		}

		feature_found = true;
		resolved_feature_name = from_wx(macro->StrDisplay(nullptr));
		AppendHostLog("macro.feature-found");

		auto *context = runtime.GetContext();
		if (!context) {
			Finish(41, "automation session context is unavailable");
			return;
		}

		if (macro->Type() & cmd::COMMAND_VALIDATE) {
			validate_ran = true;
			AppendHostLog("macro.validate.begin");
			validate_passed = macro->Validate(context);
			AppendHostLog(std::string("macro.validate.end.") + (validate_passed ? "true" : "false"));
			if (!validate_passed) {
				Finish(41, "automation macro validation returned false");
				return;
			}
		}
		else {
			validate_passed = true;
		}

		AppendHostLog("macro.run.begin");
		(*macro)(context);
		AppendHostLog("macro.run.end");
		output_summary = SummarizeAssFile(*runtime.GetCore().ass);

		if (!request.output_subtitle_path.empty()) {
			AppendHostLog("macro.save-output.begin");
			if (!request.output_subtitle_path.parent_path().empty())
				agi::fs::CreateDirectory(request.output_subtitle_path.parent_path());
			runtime.GetCore().subsController->Save(request.output_subtitle_path, request.output_encoding);
			output_saved = true;
			AppendHostLog("macro.save-output.end");
		}

		Finish(0, "automation macro completed");
	}

	void ExecuteExportFilter(Automation4::Script& script) {
		auto *filter = FindFilter(script, request.feature_name);
		if (!filter) {
			Finish(42, "automation export filter not found in script: " + request.feature_name);
			return;
		}

		feature_found = true;
		validate_passed = true;
		resolved_feature_name = filter->GetName();
		AppendHostLog("filter.feature-found");

		auto core = runtime.GetCore();
		AssFile working_copy(*core.ass);
		AppendHostLog("filter.run.begin");
		filter->ProcessSubs(&working_copy, nullptr);
		AppendHostLog("filter.run.end");
		output_summary = SummarizeAssFile(working_copy);

		if (!request.output_subtitle_path.empty()) {
			AppendHostLog("filter.save-output.begin");
			std::string save_error;
			if (!SaveSubtitleFile(
				working_copy,
				request.output_subtitle_path,
				core.project->Timecodes(),
				request.output_encoding,
				core.singleChoiceInteractionSink,
				save_error)) {
				Finish(42, save_error.empty() ? "failed to save automation filter output" : save_error);
				return;
			}
			output_saved = true;
			AppendHostLog("filter.save-output.end");
		}

		Finish(0, "automation export filter completed");
	}

	void Execute() {
		try {
			AppendHostLog("execute.begin");
			if (!OpenInitialProjectState())
				return;

			auto core = runtime.GetCore();
			AppendHostLog("script.load.begin");
			std::string load_error;
			auto const persisted_automation_scripts = core.ass->Properties.automation_scripts;
			auto *script = EnsureScriptLoaded(core, request.script_path, &load_error);
			// --script selects code for this invocation; it does not attach that
			// script to the subtitle document being processed.
			core.ass->Properties.automation_scripts = persisted_automation_scripts;
			if (!script) {
				Finish(40, load_error.empty() ? "could not load automation script: " + ToGenericString(request.script_path) : load_error);
				return;
			}
			AppendHostLog("script.load.end");

			script_loaded = script->GetLoadedState();
			engine_name = script->GetEngineName();
			script_name = script->GetName();
			script->SetAutomationHost(Automation4::CreateAutomationLiveHost(runtime.GetContext(), mutation_journal));
			script->SetRuntimeTraceSink(&journal);
			if (debug_session) {
				debug_session->SetTarget({
					engine_name,
					request.script_path,
					request.feature_name
				});
				script->SetDebugSession(debug_session.get());
			}
			AppendHostLog(std::string("script.loaded-state.") + (script_loaded ? "true" : "false"));

			if (!script_loaded) {
				Finish(40, script->GetDescription().empty() ? "automation script failed to load" : script->GetDescription());
				return;
			}

			switch (request.feature_kind) {
			case AutomationSessionFeatureKind::Macro:
				ExecuteMacro(*script);
				return;
			case AutomationSessionFeatureKind::ExportFilter:
				ExecuteExportFilter(*script);
				return;
			}

			Finish(40, "unsupported automation session feature kind");
		}
		catch (agi::UserCancelException const&) {
			Finish(43, "automation session cancelled");
		}
		catch (agi::Exception const& e) {
			Finish(43, e.GetMessage());
		}
		catch (std::exception const& e) {
			Finish(43, e.what());
		}
		catch (...) {
			Finish(43, "unhandled exception during automation session");
		}
	}

public:
	Runner(AutomationSessionRequest request, std::function<void(AutomationSessionResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done))
	, runtime(PlaybackSessionHostOptions{
		this->request.video_provider,
		this->request.audio_provider,
		this->request.trace_dir,
		this->request.audio_rate_scale,
		this->request.audio_quantum_ms,
		"headless-automation-session-%%%%%%%%",
		{
			this->request.video_track_index,
			this->request.audio_track_index,
			this->request.subtitle_track_index,
			true
		}
	}) {
		if (this->request.debug_session)
			debug_session = this->request.debug_session;
		else if (this->request.debug.enabled) {
			auto debug_request = this->request.debug;
			debug_request.nonblocking = true;
			debug_session = std::make_shared<Automation4::AutomationDebugSession>(std::move(debug_request));
		}
	}

	void Start() {
		if (request.script_path.empty()) {
			Finish(40, "automation session requires a script path");
			return;
		}
		if (request.feature_name.empty()) {
			Finish(40, "automation session requires a feature name");
			return;
		}

		int error_code = 0;
		std::string error_message;
		host_started = runtime.Start(error_code, error_message);
		if (!host_started) {
			Finish(error_code ? error_code : 2,
				error_message.empty() ? "failed to start automation session host" : error_message);
			return;
		}

		Execute();
	}
};

}

void RunAsync(AutomationSessionRequest request, std::function<void(AutomationSessionResult)> on_done) {
	auto runner = std::make_unique<Runner>(std::move(request), std::move(on_done));
	runner->Start();
}

}
