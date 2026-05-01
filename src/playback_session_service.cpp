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

#include "playback_session_service.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_controller.h"
#include "headless_playback_session_host.h"
#include "include/aegisub/context.h"
#include "playback_session_timer.h"
#include "project_open_service.h"
#include "selection_controller.h"
#include "video_controller.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aegisub::playback_session_service {
namespace {

using headless_playback_session_host::BoolString;
using headless_playback_session_host::DescribeProviderFallback;
using headless_playback_session_host::FormatProviderAttempts;
using headless_playback_session_host::PlaybackSessionHost;
using headless_playback_session_host::PlaybackSessionHostOptions;
using headless_playback_session_host::UsedProviderFallback;
using playback_query_service::PlaybackStateSnapshot;
using playback_query_service::ProjectMediaSnapshot;

struct QueryRecord {
	size_t step_index = 0;
	std::string kind;
	std::optional<ProjectMediaSnapshot> media;
	std::optional<PlaybackStateSnapshot> playback;
};

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

std::string StepName(PlaybackSessionStepKind kind) {
	switch (kind) {
	case PlaybackSessionStepKind::OpenMedia: return "open";
	case PlaybackSessionStepKind::ReopenMedia: return "reopen";
	case PlaybackSessionStepKind::CloseMedia: return "close";
	case PlaybackSessionStepKind::InstallPlayLine: return "install-playline";
	case PlaybackSessionStepKind::PlayVideo: return "play";
	case PlaybackSessionStepKind::PlayLine: return "playline";
	case PlaybackSessionStepKind::StopPlayback: return "stop";
	case PlaybackSessionStepKind::Sleep: return "sleep";
	case PlaybackSessionStepKind::WaitPlaybackStop: return "wait-playback-stop";
	case PlaybackSessionStepKind::JumpToTime: return "jump-time";
	case PlaybackSessionStepKind::JumpToFrame: return "jump-frame";
	case PlaybackSessionStepKind::QueryMedia: return "query-media";
	case PlaybackSessionStepKind::QueryPlayback: return "query-playback";
	case PlaybackSessionStepKind::AssertMedia: return "assert-media";
	case PlaybackSessionStepKind::AssertPlaying: return "assert-playing";
	case PlaybackSessionStepKind::AssertAuthority: return "assert-authority";
	}
	return "unknown";
}

std::string DescribeAuthority(PlaybackAuthorityKind authority) {
	return authority == PlaybackAuthorityKind::Audio ? "audio" : "video";
}

std::string Sanitize(std::string const& value) {
	return provider_selection_diagnostics::SanitizeText(value);
}

class Runner final {
	PlaybackSessionRequest request;
	std::function<void(PlaybackSessionResult)> on_done;
	PlaybackSessionHost runtime;
	std::unique_ptr<PlaybackSessionTimer> timer;
	std::chrono::steady_clock::time_point wait_deadline = std::chrono::steady_clock::now();
	size_t next_step = 0;
	bool finished = false;
	bool host_started = false;
	std::vector<QueryRecord> queries;
	size_t open_count = 0;
	size_t reopen_count = 0;
	size_t close_count = 0;
	size_t query_count = 0;
	size_t play_count = 0;
	size_t playline_count = 0;
	size_t stop_count = 0;
	size_t jump_time_count = 0;
	size_t jump_frame_count = 0;

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

	void RecordMediaQuery(size_t step_index, std::string kind, ProjectMediaSnapshot snapshot) {
		queries.push_back(QueryRecord{
			step_index,
			std::move(kind),
			std::move(snapshot),
			std::nullopt,
		});
		++query_count;
	}

	void RecordPlaybackQuery(size_t step_index, std::string kind, PlaybackStateSnapshot snapshot) {
		queries.push_back(QueryRecord{
			step_index,
			std::move(kind),
			std::nullopt,
			std::move(snapshot),
		});
		++query_count;
	}

	void InstallProbeLine(int start_ms, int duration_ms) {
		auto core = runtime.GetCore();
		core.ass->Events.clear_and_dispose([](AssDialogue* line) { delete line; });

		auto* line = new AssDialogue;
		line->Row = 0;
		line->Start = start_ms;
		line->End = start_ms + duration_ms;
		line->Text = "headless playback session";
		core.ass->Events.push_back(*line);
		core.selectionController->SetSelectionAndActive({line}, line);
		core.ass->Commit("headless playback session", AssFile::COMMIT_NEW);
	}

	bool IsPlaybackActive() {
		auto core = runtime.GetCore();
		return (core.videoController && core.videoController->IsPlaying())
			|| (core.audioController && core.audioController->IsPlaying());
	}

	void WriteManifest(PlaybackSessionResult const& result) const {
		auto out = agi::io::OpenOutputFileStream(runtime.TraceDir() / "manifest.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		out << "command=session playback\n";
		out << "session.video=" << Sanitize(ToGenericString(request.video_path)) << "\n";
		out << "session.audio=" << Sanitize(ToGenericString(request.audio_path)) << "\n";
		out << "session.skip_audio=" << BoolString(request.skip_audio) << "\n";
		out << "session.total_steps=" << result.total_steps << "\n";
		for (size_t i = 0; i < request.steps.size(); ++i)
			out << "session.step." << (i + 1) << "=" << Sanitize(request.steps[i].source_text) << "\n";
	}

	void WriteSummary(PlaybackSessionResult const& result) const {
		auto out = agi::io::OpenOutputFileStream(runtime.TraceDir() / "summary.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		auto write_value = [&](std::string const& key, std::string const& value) {
			out << key << "=" << Sanitize(value) << "\n";
		};
		auto write_bool = [&](std::string const& key, bool value) {
			out << key << "=" << BoolString(value) << "\n";
		};

		out << "session.completed_steps=" << result.completed_steps << "\n";
		out << "session.total_steps=" << result.total_steps << "\n";
		out << "session.open_count=" << result.open_count << "\n";
		out << "session.reopen_count=" << result.reopen_count << "\n";
		out << "session.close_count=" << result.close_count << "\n";
		out << "session.query_count=" << result.query_count << "\n";
		out << "session.play_count=" << result.play_count << "\n";
		out << "session.playline_count=" << result.playline_count << "\n";
		out << "session.stop_count=" << result.stop_count << "\n";
		out << "session.jump_time_count=" << result.jump_time_count << "\n";
		out << "session.jump_frame_count=" << result.jump_frame_count << "\n";
		write_value("session.selected.video_provider", result.selected_video_provider);
		write_value("session.actual.video_provider", result.actual_video_provider);
		write_value("session.actual.video_decoder", result.actual_video_decoder);
		write_bool("session.video.provider_fallback", result.video_provider_fallback);
		write_value("session.video.provider_fallback_reason", result.video_provider_fallback_reason);
		write_value("session.video.provider_attempts", result.video_provider_attempts);
		write_value("session.selected.audio_provider", result.selected_audio_provider);
		write_value("session.actual.audio_provider_factory", result.actual_audio_provider_factory);
		write_value("session.actual.audio_provider", result.actual_audio_provider);
		write_bool("session.audio.provider_fallback", result.audio_provider_fallback);
		write_value("session.audio.provider_fallback_reason", result.audio_provider_fallback_reason);
		write_value("session.audio.provider_attempts", result.audio_provider_attempts);
		write_bool("session.final.has_video", result.final_media.has_video);
		write_bool("session.final.has_audio", result.final_media.has_audio);
		write_bool("session.final.video_playing", result.final_playback.video_playing);
		write_bool("session.final.audio_playing", result.final_playback.audio_playing);
		write_bool("session.final.playback_uses_audio_authority", result.final_playback.playback_uses_audio_authority);
		out << "session.final.current_frame=" << result.final_playback.current_frame << "\n";
		out << "session.final.current_video_time_ms=" << result.final_playback.current_video_time_ms << "\n";
		out << "session.final.current_audio_time_ms=" << result.final_playback.current_audio_time_ms << "\n";
		out << "session.final.video_duration_ms=" << result.final_media.video_duration_ms << "\n";
		out << "session.final.audio_duration_ms=" << result.final_media.audio_duration_ms << "\n";
		out << "session.result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		write_value("session.message", result.message);

		for (size_t i = 0; i < queries.size(); ++i) {
			auto const prefix = "session.query." + std::to_string(i + 1);
			auto const& query = queries[i];
			out << prefix << ".step=" << query.step_index << "\n";
			write_value(prefix + ".kind", query.kind);
			if (query.media) {
				write_bool(prefix + ".media.has_video", query.media->has_video);
				write_bool(prefix + ".media.has_audio", query.media->has_audio);
				out << prefix << ".media.video_duration_ms=" << query.media->video_duration_ms << "\n";
				out << prefix << ".media.audio_duration_ms=" << query.media->audio_duration_ms << "\n";
				write_value(prefix + ".media.video_decoder_name", query.media->video_decoder_name);
				write_value(prefix + ".media.audio_provider_name", query.media->audio_provider_name);
			}
			if (query.playback) {
				write_bool(prefix + ".playback.video_playing", query.playback->video_playing);
				write_bool(prefix + ".playback.audio_playing", query.playback->audio_playing);
				write_bool(prefix + ".playback.uses_audio_authority", query.playback->playback_uses_audio_authority);
				out << prefix << ".playback.current_frame=" << query.playback->current_frame << "\n";
				out << prefix << ".playback.current_video_time_ms=" << query.playback->current_video_time_ms << "\n";
				out << prefix << ".playback.current_audio_time_ms=" << query.playback->current_audio_time_ms << "\n";
				out << prefix << ".playback.primary_begin_ms=" << query.playback->primary_playback_begin_ms << "\n";
				out << prefix << ".playback.primary_end_ms=" << query.playback->primary_playback_end_ms << "\n";
			}
		}
	}

	void PrintReport(PlaybackSessionResult const& result) const {
		std::cout << "headless-playback-session\n";
		std::cout << "video=" << agi::fs::PathToGenericString(request.video_path) << "\n";
		std::cout << "audio=" << agi::fs::PathToGenericString(request.audio_path) << "\n";
		std::cout << "skip_audio=" << BoolString(request.skip_audio) << "\n";
		std::cout << "selected.video_provider=" << result.selected_video_provider << "\n";
		std::cout << "selected.audio_provider=" << result.selected_audio_provider << "\n";
		std::cout << "actual.video_provider=" << result.actual_video_provider << "\n";
		std::cout << "actual.video_decoder=" << result.actual_video_decoder << "\n";
		std::cout << "video.provider_fallback=" << BoolString(result.video_provider_fallback) << "\n";
		std::cout << "video.provider_fallback_reason=" << result.video_provider_fallback_reason << "\n";
		std::cout << "video.provider_attempts=" << result.video_provider_attempts << "\n";
		std::cout << "actual.audio_provider_factory=" << result.actual_audio_provider_factory << "\n";
		std::cout << "actual.audio_provider=" << result.actual_audio_provider << "\n";
		std::cout << "audio.provider_fallback=" << BoolString(result.audio_provider_fallback) << "\n";
		std::cout << "audio.provider_fallback_reason=" << result.audio_provider_fallback_reason << "\n";
		std::cout << "audio.provider_attempts=" << result.audio_provider_attempts << "\n";
		std::cout << "steps.total=" << result.total_steps << "\n";
		std::cout << "steps.completed=" << result.completed_steps << "\n";
		std::cout << "open_count=" << result.open_count << "\n";
		std::cout << "reopen_count=" << result.reopen_count << "\n";
		std::cout << "close_count=" << result.close_count << "\n";
		std::cout << "query_count=" << result.query_count << "\n";
		std::cout << "play_count=" << result.play_count << "\n";
		std::cout << "playline_count=" << result.playline_count << "\n";
		std::cout << "stop_count=" << result.stop_count << "\n";
		std::cout << "jump_time_count=" << result.jump_time_count << "\n";
		std::cout << "jump_frame_count=" << result.jump_frame_count << "\n";
		std::cout << "final.has_video=" << BoolString(result.final_media.has_video) << "\n";
		std::cout << "final.has_audio=" << BoolString(result.final_media.has_audio) << "\n";
		std::cout << "final.video_playing=" << BoolString(result.final_playback.video_playing) << "\n";
		std::cout << "final.audio_playing=" << BoolString(result.final_playback.audio_playing) << "\n";
		std::cout << "final.playback_uses_audio_authority=" << BoolString(result.final_playback.playback_uses_audio_authority) << "\n";
		std::cout << "final.current_frame=" << result.final_playback.current_frame << "\n";
		std::cout << "final.current_video_time_ms=" << result.final_playback.current_video_time_ms << "\n";
		std::cout << "final.current_audio_time_ms=" << result.final_playback.current_audio_time_ms << "\n";
		for (size_t i = 0; i < queries.size(); ++i) {
			auto const& query = queries[i];
			std::cout << "query[" << (i + 1) << "].step=" << query.step_index << "\n";
			std::cout << "query[" << (i + 1) << "].kind=" << query.kind << "\n";
			if (query.media) {
				std::cout << "query[" << (i + 1) << "].media.has_video=" << BoolString(query.media->has_video) << "\n";
				std::cout << "query[" << (i + 1) << "].media.has_audio=" << BoolString(query.media->has_audio) << "\n";
				std::cout << "query[" << (i + 1) << "].media.video_duration_ms=" << query.media->video_duration_ms << "\n";
				std::cout << "query[" << (i + 1) << "].media.audio_duration_ms=" << query.media->audio_duration_ms << "\n";
			}
			if (query.playback) {
				std::cout << "query[" << (i + 1) << "].playback.video_playing=" << BoolString(query.playback->video_playing) << "\n";
				std::cout << "query[" << (i + 1) << "].playback.audio_playing=" << BoolString(query.playback->audio_playing) << "\n";
				std::cout << "query[" << (i + 1) << "].playback.uses_audio_authority=" << BoolString(query.playback->playback_uses_audio_authority) << "\n";
				std::cout << "query[" << (i + 1) << "].playback.current_frame=" << query.playback->current_frame << "\n";
				std::cout << "query[" << (i + 1) << "].playback.current_video_time_ms=" << query.playback->current_video_time_ms << "\n";
				std::cout << "query[" << (i + 1) << "].playback.current_audio_time_ms=" << query.playback->current_audio_time_ms << "\n";
			}
		}
		std::cout << "trace_dir=" << agi::fs::PathToGenericString(result.trace_dir) << "\n";
		std::cout << "result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		if (!result.message.empty())
			std::cout << "message=" << result.message << "\n";
	}

	PlaybackSessionResult BuildResult(int exit_code, std::string const& message) {
		PlaybackSessionResult result;
		result.exit_code = exit_code;
		result.passed = exit_code == 0;
		result.total_steps = request.steps.size();
		result.completed_steps = next_step;
		result.open_count = open_count;
		result.reopen_count = reopen_count;
		result.close_count = close_count;
		result.query_count = query_count;
		result.play_count = play_count;
		result.playline_count = playline_count;
		result.stop_count = stop_count;
		result.jump_time_count = jump_time_count;
		result.jump_frame_count = jump_frame_count;
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
		if (host_started) {
			auto core = runtime.GetCore();
			result.final_media = playback_query_service::QueryProjectMedia(core);
			result.final_playback = playback_query_service::QueryPlaybackState(core);
		}
		return result;
	}

	void Finish(int exit_code, std::string message) {
		if (finished)
			return;
		finished = true;
		timer->StopAll();

		auto result = BuildResult(exit_code, std::move(message));
		runtime.CloseMedia();
		runtime.ShutdownTrace();
		WriteManifest(result);
		WriteSummary(result);
		PrintReport(result);
		runtime.ReleaseResources();

		if (on_done)
			on_done(std::move(result));
		delete this;
	}

	void FailStep(size_t step_index, PlaybackSessionStep const& step, std::string const& detail) {
		std::ostringstream out;
		out << "session step " << step_index << " (" << StepName(step.kind) << ") failed";
		if (!step.source_text.empty())
			out << ": " << step.source_text;
		if (!detail.empty())
			out << " | " << detail;
		Finish(20, out.str());
	}

	bool ExecuteStep(size_t step_index, PlaybackSessionStep const& step) {
		auto const current_step = step_index + 1;
		switch (step.kind) {
		case PlaybackSessionStepKind::OpenMedia: {
			auto result = runtime.OpenMedia(BuildOpenOptions());
			if (!result.opened) {
				FailStep(current_step, step, result.error.empty() ? "open failed" : result.error);
				return false;
			}
			++open_count;
			return true;
		}
		case PlaybackSessionStepKind::ReopenMedia: {
			auto result = runtime.ReopenMedia();
			if (!result.opened) {
				FailStep(current_step, step, result.error.empty() ? "reopen failed" : result.error);
				return false;
			}
			++reopen_count;
			return true;
		}
		case PlaybackSessionStepKind::CloseMedia: {
			runtime.CloseMedia();
			auto snapshot = playback_query_service::QueryProjectMedia(runtime.GetCore());
			if (snapshot.has_video || snapshot.has_audio) {
				FailStep(current_step, step, "media still opened after close");
				return false;
			}
			++close_count;
			return true;
		}
		case PlaybackSessionStepKind::InstallPlayLine:
			InstallProbeLine(step.primary_value, step.secondary_value);
			return true;
		case PlaybackSessionStepKind::PlayVideo: {
			auto core = runtime.GetCore();
			core.videoController->Play();
			if (!core.videoController->IsPlaying()) {
				FailStep(current_step, step, "video controller did not enter playback");
				return false;
			}
			++play_count;
			return true;
		}
		case PlaybackSessionStepKind::PlayLine: {
			auto core = runtime.GetCore();
			core.videoController->PlayLine();
			if (!core.videoController->IsPlaying()) {
				FailStep(current_step, step, "video controller did not enter line playback");
				return false;
			}
			++playline_count;
			return true;
		}
		case PlaybackSessionStepKind::StopPlayback: {
			auto core = runtime.GetCore();
			if (core.videoController->IsPlaying())
				core.videoController->Stop();
			if (core.audioController->IsPlaying())
				core.audioController->Stop();
			auto snapshot = playback_query_service::QueryPlaybackState(core);
			if (snapshot.video_playing || snapshot.audio_playing) {
				FailStep(current_step, step, "playback still active after stop");
				return false;
			}
			++stop_count;
			return true;
		}
		case PlaybackSessionStepKind::Sleep:
			timer->StartDelayOnce(step.primary_value);
			return false;
		case PlaybackSessionStepKind::WaitPlaybackStop:
			if (!IsPlaybackActive())
				return true;
			wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(step.primary_value);
			timer->StartWaitPolling(20);
			return false;
		case PlaybackSessionStepKind::JumpToTime: {
			auto core = runtime.GetCore();
			bool const was_playing = IsPlaybackActive();
			core.videoController->JumpToTime(step.primary_value, agi::vfr::EXACT);
			if (was_playing && !IsPlaybackActive()) {
				FailStep(current_step, step, "playback did not survive jump-time");
				return false;
			}
			++jump_time_count;
			return true;
		}
		case PlaybackSessionStepKind::JumpToFrame: {
			auto core = runtime.GetCore();
			bool const was_playing = IsPlaybackActive();
			core.videoController->JumpToFrame(step.primary_value);
			if (was_playing && !IsPlaybackActive()) {
				FailStep(current_step, step, "playback did not survive jump-frame");
				return false;
			}
			++jump_frame_count;
			return true;
		}
		case PlaybackSessionStepKind::QueryMedia: {
			auto snapshot = playback_query_service::QueryProjectMedia(runtime.GetCore());
			RecordMediaQuery(current_step, "media", std::move(snapshot));
			return true;
		}
		case PlaybackSessionStepKind::QueryPlayback: {
			auto snapshot = playback_query_service::QueryPlaybackState(runtime.GetCore());
			RecordPlaybackQuery(current_step, "playback", std::move(snapshot));
			return true;
		}
		case PlaybackSessionStepKind::AssertMedia: {
			auto snapshot = playback_query_service::QueryProjectMedia(runtime.GetCore());
			RecordMediaQuery(current_step, "assert-media", snapshot);
			if (snapshot.has_video != step.expected_first || snapshot.has_audio != step.expected_second) {
				FailStep(current_step, step, "media assertion mismatch");
				return false;
			}
			return true;
		}
		case PlaybackSessionStepKind::AssertPlaying: {
			auto snapshot = playback_query_service::QueryPlaybackState(runtime.GetCore());
			RecordPlaybackQuery(current_step, "assert-playing", snapshot);
			if (snapshot.video_playing != step.expected_first || snapshot.audio_playing != step.expected_second) {
				FailStep(current_step, step, "playback assertion mismatch");
				return false;
			}
			return true;
		}
		case PlaybackSessionStepKind::AssertAuthority: {
			auto snapshot = playback_query_service::QueryPlaybackState(runtime.GetCore());
			RecordPlaybackQuery(current_step, "assert-authority", snapshot);
			auto const expected = step.expected_authority == PlaybackAuthorityKind::Audio;
			if (snapshot.playback_uses_audio_authority != expected) {
				FailStep(current_step, step, "authority assertion mismatch");
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

	void OnDelayTimer() {
		if (finished)
			return;
		Advance();
	}

	void OnWaitTimer() {
		if (finished)
			return;
		if (!IsPlaybackActive()) {
			timer->StopWaitPolling();
			Advance();
			return;
		}
		if (std::chrono::steady_clock::now() >= wait_deadline) {
			timer->StopWaitPolling();
			auto const& step = request.steps[next_step - 1];
			FailStep(next_step, step, "timed out waiting for playback stop");
		}
	}

public:
	Runner(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done))
	, runtime(PlaybackSessionHostOptions{
		this->request.video_provider,
		this->request.audio_provider,
		this->request.trace_dir,
		this->request.audio_rate_scale,
		this->request.audio_quantum_ms,
		"headless-playback-session-%%%%%%%%",
		{
			this->request.video_track_index,
			this->request.audio_track_index,
			{},
			true
		}
	})
	, timer(CreatePlaybackSessionTimer(
		[this] { OnDelayTimer(); },
		[this] { OnWaitTimer(); })) {
	}

	void Start() {
		if (request.steps.empty()) {
			Finish(21, "session script is empty");
			return;
		}

		int error_code = 0;
		std::string error_message;
		host_started = runtime.Start(error_code, error_message);
		if (!host_started) {
			Finish(error_code ? error_code : 2,
				error_message.empty() ? "failed to start playback session host" : error_message);
			return;
		}

		Advance();
	}
};

}

void RunAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done) {
	auto* runner = new Runner(std::move(request), std::move(on_done));
	runner->Start();
}

}
