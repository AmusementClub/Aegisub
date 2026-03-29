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
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "project_session_service.h"

#include "headless_playback_session_host.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "project_open_service.h"
#include "provider_selection_diagnostics.h"
#include "subs_controller.h"

#include <libaegisub/fs.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aegisub::project_session_service {
namespace {

using headless_playback_session_host::BoolString;
using headless_playback_session_host::DescribeProviderFallback;
using headless_playback_session_host::FormatProviderAttempts;
using headless_playback_session_host::PlaybackSessionHost;
using headless_playback_session_host::PlaybackSessionHostOptions;
using headless_playback_session_host::UsedProviderFallback;
using project_query_service::ProjectSessionSnapshot;

struct QueryRecord {
	size_t step_index = 0;
	std::string kind;
	ProjectSessionSnapshot snapshot;
};

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

std::string Sanitize(std::string const& value) {
	return provider_selection_diagnostics::SanitizeText(value);
}

std::string StepName(ProjectSessionStepKind kind) {
	switch (kind) {
	case ProjectSessionStepKind::OpenMedia: return "open-media";
	case ProjectSessionStepKind::ReopenMedia: return "reopen-media";
	case ProjectSessionStepKind::CloseMedia: return "close-media";
	case ProjectSessionStepKind::OpenSubtitles: return "open-subtitles";
	case ProjectSessionStepKind::OpenSubtitlesUnlinked: return "open-subtitles-unlinked";
	case ProjectSessionStepKind::OpenSubtitlesFromVideo: return "open-subtitles-from-video";
	case ProjectSessionStepKind::CloseSubtitles: return "close-subtitles";
	case ProjectSessionStepKind::OpenTimecodes: return "open-timecodes";
	case ProjectSessionStepKind::CloseTimecodes: return "close-timecodes";
	case ProjectSessionStepKind::OpenKeyframes: return "open-keyframes";
	case ProjectSessionStepKind::CloseKeyframes: return "close-keyframes";
	case ProjectSessionStepKind::QueryProject: return "query-project";
	case ProjectSessionStepKind::AssertProject: return "assert-project";
	case ProjectSessionStepKind::AssertSubtitleCounts: return "assert-subtitle-counts";
	case ProjectSessionStepKind::AssertSubtitleModified: return "assert-subtitle-modified";
	}
	return "unknown";
}

class Runner final {
	ProjectSessionRequest request;
	std::function<void(ProjectSessionResult)> on_done;
	PlaybackSessionHost runtime;
	size_t next_step = 0;
	bool finished = false;
	bool host_started = false;
	std::vector<QueryRecord> queries;
	size_t open_media_count = 0;
	size_t reopen_media_count = 0;
	size_t close_media_count = 0;
	size_t open_subtitles_count = 0;
	size_t close_subtitles_count = 0;
	size_t open_timecodes_count = 0;
	size_t close_timecodes_count = 0;
	size_t open_keyframes_count = 0;
	size_t close_keyframes_count = 0;
	size_t query_count = 0;

	project_open_service::PlaybackOpenOptions BuildOpenOptions() const {
		project_open_service::PlaybackOpenOptions options;
		options.video_path = request.video_path;
		options.skip_audio = request.skip_audio;
		if (!request.skip_audio) {
			if (!request.audio_path.empty())
				options.audio_path = request.audio_path;
			else if (!request.video_path.empty())
				options.audio_path = request.video_path;
		}
		return options;
	}

	void RecordProjectQuery(size_t step_index, std::string kind, ProjectSessionSnapshot snapshot) {
		queries.push_back(QueryRecord{
			step_index,
			std::move(kind),
			std::move(snapshot),
		});
		++query_count;
	}

	bool MatchProjectExpectation(ProjectSessionSnapshot const& snapshot, ProjectSessionStep const& step, std::string& detail) const {
		if (snapshot.subtitle_file_loaded != step.expected_subtitle_file_loaded) {
			detail = "subtitle_file_loaded expected " + std::string(BoolString(step.expected_subtitle_file_loaded))
				+ " but was " + BoolString(snapshot.subtitle_file_loaded);
			return false;
		}
		if (snapshot.media.has_video != step.expected_has_video) {
			detail = "has_video expected " + std::string(BoolString(step.expected_has_video))
				+ " but was " + BoolString(snapshot.media.has_video);
			return false;
		}
		if (snapshot.media.has_audio != step.expected_has_audio) {
			detail = "has_audio expected " + std::string(BoolString(step.expected_has_audio))
				+ " but was " + BoolString(snapshot.media.has_audio);
			return false;
		}
		if (snapshot.timecodes_file_loaded != step.expected_timecodes_file_loaded) {
			detail = "timecodes_file_loaded expected " + std::string(BoolString(step.expected_timecodes_file_loaded))
				+ " but was " + BoolString(snapshot.timecodes_file_loaded);
			return false;
		}
		if (snapshot.keyframes_file_loaded != step.expected_keyframes_file_loaded) {
			detail = "keyframes_file_loaded expected " + std::string(BoolString(step.expected_keyframes_file_loaded))
				+ " but was " + BoolString(snapshot.keyframes_file_loaded);
			return false;
		}
		return true;
	}

	void WriteSnapshot(std::ofstream& out, std::string const& prefix, ProjectSessionSnapshot const& snapshot) const {
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

	void PrintSnapshot(ProjectSessionSnapshot const& snapshot, std::string const& prefix) const {
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

	void WriteManifest(ProjectSessionResult const& result) const {
		if (runtime.TraceDir().empty())
			return;
		std::ofstream out(runtime.TraceDir() / "manifest.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		out << "command=session project\n";
		out << "session.video=" << Sanitize(ToGenericString(request.video_path)) << "\n";
		out << "session.audio=" << Sanitize(ToGenericString(request.audio_path)) << "\n";
		out << "session.subtitle=" << Sanitize(ToGenericString(request.subtitle_path)) << "\n";
		out << "session.timecodes=" << Sanitize(ToGenericString(request.timecodes_path)) << "\n";
		out << "session.keyframes=" << Sanitize(ToGenericString(request.keyframes_path)) << "\n";
		out << "session.subtitle_encoding=" << Sanitize(request.subtitle_encoding) << "\n";
		out << "session.skip_audio=" << BoolString(request.skip_audio) << "\n";
		out << "session.total_steps=" << result.total_steps << "\n";
		for (size_t i = 0; i < request.steps.size(); ++i)
			out << "session.step." << (i + 1) << "=" << Sanitize(request.steps[i].source_text) << "\n";
	}

	void WriteSummary(ProjectSessionResult const& result) const {
		if (runtime.TraceDir().empty())
			return;
		std::ofstream out(runtime.TraceDir() / "summary.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		auto write_value = [&](std::string const& key, std::string const& value) {
			out << key << "=" << Sanitize(value) << "\n";
		};
		auto write_bool = [&](std::string const& key, bool value) {
			out << key << "=" << BoolString(value) << "\n";
		};

		out << "project.completed_steps=" << result.completed_steps << "\n";
		out << "project.total_steps=" << result.total_steps << "\n";
		out << "project.open_media_count=" << result.open_media_count << "\n";
		out << "project.reopen_media_count=" << result.reopen_media_count << "\n";
		out << "project.close_media_count=" << result.close_media_count << "\n";
		out << "project.open_subtitles_count=" << result.open_subtitles_count << "\n";
		out << "project.close_subtitles_count=" << result.close_subtitles_count << "\n";
		out << "project.open_timecodes_count=" << result.open_timecodes_count << "\n";
		out << "project.close_timecodes_count=" << result.close_timecodes_count << "\n";
		out << "project.open_keyframes_count=" << result.open_keyframes_count << "\n";
		out << "project.close_keyframes_count=" << result.close_keyframes_count << "\n";
		out << "project.query_count=" << result.query_count << "\n";
		write_value("project.selected.video_provider", result.selected_video_provider);
		write_value("project.selected.audio_provider", result.selected_audio_provider);
		write_value("project.actual.video_provider", result.actual_video_provider);
		write_value("project.actual.video_decoder", result.actual_video_decoder);
		write_bool("project.video.provider_fallback", result.video_provider_fallback);
		write_value("project.video.provider_fallback_reason", result.video_provider_fallback_reason);
		write_value("project.video.provider_attempts", result.video_provider_attempts);
		write_value("project.actual.audio_provider_factory", result.actual_audio_provider_factory);
		write_value("project.actual.audio_provider", result.actual_audio_provider);
		write_bool("project.audio.provider_fallback", result.audio_provider_fallback);
		write_value("project.audio.provider_fallback_reason", result.audio_provider_fallback_reason);
		write_value("project.audio.provider_attempts", result.audio_provider_attempts);
		WriteSnapshot(out, "project.final", result.final_project);
		out << "project.result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		write_value("project.message", result.message);

		for (size_t i = 0; i < queries.size(); ++i) {
			auto const prefix = "project.query." + std::to_string(i + 1);
			out << prefix << ".step=" << queries[i].step_index << "\n";
			write_value(prefix + ".kind", queries[i].kind);
			WriteSnapshot(out, prefix, queries[i].snapshot);
		}
	}

	void PrintReport(ProjectSessionResult const& result) const {
		std::cout << "headless-project-session\n";
		std::cout << "video=" << ToGenericString(request.video_path) << "\n";
		std::cout << "audio=" << ToGenericString(request.audio_path) << "\n";
		std::cout << "subtitle=" << ToGenericString(request.subtitle_path) << "\n";
		std::cout << "timecodes=" << ToGenericString(request.timecodes_path) << "\n";
		std::cout << "keyframes=" << ToGenericString(request.keyframes_path) << "\n";
		std::cout << "subtitle_encoding=" << request.subtitle_encoding << "\n";
		std::cout << "skip_audio=" << BoolString(request.skip_audio) << "\n";
		std::cout << "selected.video_provider=" << result.selected_video_provider << "\n";
		std::cout << "selected.audio_provider=" << result.selected_audio_provider << "\n";
		std::cout << "actual.video_provider=" << result.actual_video_provider << "\n";
		std::cout << "actual.video_decoder=" << result.actual_video_decoder << "\n";
		std::cout << "actual.audio_provider_factory=" << result.actual_audio_provider_factory << "\n";
		std::cout << "actual.audio_provider=" << result.actual_audio_provider << "\n";
		std::cout << "steps.total=" << result.total_steps << "\n";
		std::cout << "steps.completed=" << result.completed_steps << "\n";
		std::cout << "open_media_count=" << result.open_media_count << "\n";
		std::cout << "reopen_media_count=" << result.reopen_media_count << "\n";
		std::cout << "close_media_count=" << result.close_media_count << "\n";
		std::cout << "open_subtitles_count=" << result.open_subtitles_count << "\n";
		std::cout << "close_subtitles_count=" << result.close_subtitles_count << "\n";
		std::cout << "open_timecodes_count=" << result.open_timecodes_count << "\n";
		std::cout << "close_timecodes_count=" << result.close_timecodes_count << "\n";
		std::cout << "open_keyframes_count=" << result.open_keyframes_count << "\n";
		std::cout << "close_keyframes_count=" << result.close_keyframes_count << "\n";
		std::cout << "query_count=" << result.query_count << "\n";
		PrintSnapshot(result.final_project, "final");
		for (size_t i = 0; i < queries.size(); ++i) {
			std::cout << "query[" << (i + 1) << "].step=" << queries[i].step_index << "\n";
			std::cout << "query[" << (i + 1) << "].kind=" << queries[i].kind << "\n";
			PrintSnapshot(queries[i].snapshot, "query[" + std::to_string(i + 1) + "]");
		}
		std::cout << "trace_dir=" << ToGenericString(result.trace_dir) << "\n";
		std::cout << "result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		if (!result.message.empty())
			std::cout << "message=" << result.message << "\n";
	}

	ProjectSessionResult BuildResult(int exit_code, std::string const& message) {
		ProjectSessionResult result;
		result.exit_code = exit_code;
		result.passed = exit_code == 0;
		result.total_steps = request.steps.size();
		result.completed_steps = next_step;
		result.open_media_count = open_media_count;
		result.reopen_media_count = reopen_media_count;
		result.close_media_count = close_media_count;
		result.open_subtitles_count = open_subtitles_count;
		result.close_subtitles_count = close_subtitles_count;
		result.open_timecodes_count = open_timecodes_count;
		result.close_timecodes_count = close_timecodes_count;
		result.open_keyframes_count = open_keyframes_count;
		result.close_keyframes_count = close_keyframes_count;
		result.query_count = query_count;
		result.trace_dir = runtime.TraceDir();
		result.message = message;
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
		if (host_started)
			result.final_project = project_query_service::QueryProjectSession(runtime.GetCore());
		return result;
	}

	void Finish(int exit_code, std::string message) {
		if (finished)
			return;
		finished = true;

		auto result = BuildResult(exit_code, std::move(message));
		runtime.ShutdownTrace();
		WriteManifest(result);
		WriteSummary(result);
		PrintReport(result);
		runtime.CloseMedia();
		runtime.ReleaseResources();

		if (on_done)
			on_done(std::move(result));
		delete this;
	}

	void FailStep(size_t step_index, ProjectSessionStep const& step, std::string const& detail) {
		std::ostringstream out;
		out << "project step " << step_index << " (" << StepName(step.kind) << ") failed";
		if (!step.source_text.empty())
			out << ": " << step.source_text;
		if (!detail.empty())
			out << " | " << detail;
		Finish(30, out.str());
	}

	bool ExecuteStep(size_t step_index, ProjectSessionStep const& step) {
		auto const current_step = step_index + 1;
		switch (step.kind) {
		case ProjectSessionStepKind::OpenMedia: {
			auto result = runtime.OpenMedia(BuildOpenOptions());
			if (!result.opened) {
				FailStep(current_step, step, result.error.empty() ? "open media failed" : result.error);
				return false;
			}
			++open_media_count;
			return true;
		}
		case ProjectSessionStepKind::ReopenMedia: {
			auto result = runtime.ReopenMedia();
			if (!result.opened) {
				FailStep(current_step, step, result.error.empty() ? "reopen media failed" : result.error);
				return false;
			}
			++reopen_media_count;
			return true;
		}
		case ProjectSessionStepKind::CloseMedia: {
			runtime.CloseMedia();
			auto snapshot = project_query_service::QueryProjectSession(runtime.GetCore());
			if (snapshot.media.has_video || snapshot.media.has_audio) {
				FailStep(current_step, step, "media still opened after close-media");
				return false;
			}
			++close_media_count;
			return true;
		}
		case ProjectSessionStepKind::OpenSubtitles:
		case ProjectSessionStepKind::OpenSubtitlesUnlinked: {
			if (request.subtitle_path.empty()) {
				FailStep(current_step, step, "subtitle path is required");
				return false;
			}
			auto core = runtime.GetCore();
			core.project->LoadSubtitles(
				request.subtitle_path,
				request.subtitle_encoding,
				step.kind == ProjectSessionStepKind::OpenSubtitles);
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (!snapshot.subtitle_file_loaded || snapshot.subtitle_path != request.subtitle_path) {
				FailStep(current_step, step, "subtitle file did not bind to requested path");
				return false;
			}
			++open_subtitles_count;
			return true;
		}
		case ProjectSessionStepKind::OpenSubtitlesFromVideo: {
			auto core = runtime.GetCore();
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (!snapshot.media.has_video || snapshot.media.video_path.empty()) {
				FailStep(current_step, step, "video must be opened before open-subtitles-from-video");
				return false;
			}
			if (!snapshot.media.can_load_subtitles_from_video) {
				FailStep(current_step, step, "current video does not advertise embedded subtitles");
				return false;
			}
			core.project->LoadSubtitles(snapshot.media.video_path, "binary", false);
			snapshot = project_query_service::QueryProjectSession(core);
			if (!snapshot.subtitle_file_loaded || snapshot.subtitle_path != snapshot.media.video_path) {
				FailStep(current_step, step, "subtitle file did not bind to current video path");
				return false;
			}
			++open_subtitles_count;
			return true;
		}
		case ProjectSessionStepKind::CloseSubtitles: {
			auto core = runtime.GetCore();
			core.project->CloseSubtitles();
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (snapshot.subtitle_file_loaded) {
				FailStep(current_step, step, "subtitle file still bound after close-subtitles");
				return false;
			}
			++close_subtitles_count;
			return true;
		}
		case ProjectSessionStepKind::OpenTimecodes: {
			if (request.timecodes_path.empty()) {
				FailStep(current_step, step, "timecodes path is required");
				return false;
			}
			auto core = runtime.GetCore();
			core.project->LoadTimecodes(request.timecodes_path);
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (!snapshot.timecodes_file_loaded || snapshot.timecodes_path != request.timecodes_path) {
				FailStep(current_step, step, "timecodes file did not bind to requested path");
				return false;
			}
			++open_timecodes_count;
			return true;
		}
		case ProjectSessionStepKind::CloseTimecodes: {
			auto core = runtime.GetCore();
			core.project->CloseTimecodes();
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (snapshot.timecodes_file_loaded) {
				FailStep(current_step, step, "timecodes file still bound after close-timecodes");
				return false;
			}
			++close_timecodes_count;
			return true;
		}
		case ProjectSessionStepKind::OpenKeyframes: {
			if (request.keyframes_path.empty()) {
				FailStep(current_step, step, "keyframes path is required");
				return false;
			}
			auto core = runtime.GetCore();
			core.project->LoadKeyframes(request.keyframes_path);
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (!snapshot.keyframes_file_loaded || snapshot.keyframes_path != request.keyframes_path) {
				FailStep(current_step, step, "keyframes file did not bind to requested path");
				return false;
			}
			++open_keyframes_count;
			return true;
		}
		case ProjectSessionStepKind::CloseKeyframes: {
			auto core = runtime.GetCore();
			core.project->CloseKeyframes();
			auto snapshot = project_query_service::QueryProjectSession(core);
			if (snapshot.keyframes_file_loaded) {
				FailStep(current_step, step, "keyframes file still bound after close-keyframes");
				return false;
			}
			++close_keyframes_count;
			return true;
		}
		case ProjectSessionStepKind::QueryProject: {
			RecordProjectQuery(current_step, "project", project_query_service::QueryProjectSession(runtime.GetCore()));
			return true;
		}
		case ProjectSessionStepKind::AssertProject: {
			auto snapshot = project_query_service::QueryProjectSession(runtime.GetCore());
			RecordProjectQuery(current_step, "assert-project", snapshot);
			std::string detail;
			if (!MatchProjectExpectation(snapshot, step, detail)) {
				FailStep(current_step, step, detail);
				return false;
			}
			return true;
		}
		case ProjectSessionStepKind::AssertSubtitleCounts: {
			auto snapshot = project_query_service::QueryProjectSession(runtime.GetCore());
			RecordProjectQuery(current_step, "assert-subtitle-counts", snapshot);
			if (snapshot.style_count != static_cast<size_t>(step.primary_value)
				|| snapshot.event_count != static_cast<size_t>(step.secondary_value)
				|| snapshot.dialogue_count != static_cast<size_t>(step.tertiary_value)
				|| snapshot.comment_count != static_cast<size_t>(step.quaternary_value)) {
				FailStep(current_step, step, "subtitle count assertion mismatch");
				return false;
			}
			return true;
		}
		case ProjectSessionStepKind::AssertSubtitleModified: {
			auto snapshot = project_query_service::QueryProjectSession(runtime.GetCore());
			RecordProjectQuery(current_step, "assert-subtitle-modified", snapshot);
			if (snapshot.subtitle_modified != step.expected_subtitle_modified) {
				FailStep(
					current_step,
					step,
					"subtitle_modified expected " + std::string(BoolString(step.expected_subtitle_modified))
						+ " but was " + BoolString(snapshot.subtitle_modified));
				return false;
			}
			return true;
		}
		}

		FailStep(current_step, step, "unknown step kind");
		return false;
	}

	void Advance() {
		while (!finished && next_step < request.steps.size()) {
			auto const step_index = next_step;
			auto const step = request.steps[step_index];
			++next_step;
			if (!ExecuteStep(step_index, step))
				return;
		}

		if (!finished)
			Finish(0, {});
	}

public:
	Runner(ProjectSessionRequest request, std::function<void(ProjectSessionResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done))
	, runtime(PlaybackSessionHostOptions{
		this->request.video_provider,
		this->request.audio_provider,
		this->request.trace_dir,
		this->request.audio_rate_scale,
		this->request.audio_quantum_ms,
		"headless-project-session-%%%%%%%%",
	}) {
	}

	void Start() {
		if (request.steps.empty()) {
			Finish(31, "project session script is empty");
			return;
		}

		int error_code = 0;
		std::string error_message;
		host_started = runtime.Start(error_code, error_message);
		if (!host_started) {
			Finish(error_code ? error_code : 2,
				error_message.empty() ? "failed to start project session host" : error_message);
			return;
		}

		Advance();
	}
};

}

void RunAsync(ProjectSessionRequest request, std::function<void(ProjectSessionResult)> on_done) {
	auto* runner = new Runner(std::move(request), std::move(on_done));
	runner->Start();
}

}
