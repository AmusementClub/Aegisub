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

#include "playback_probe_service.h"
#include "headless_playback_session_host.h"
#include "playback_probe_timer_host.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "include/aegisub/context.h"
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace aegisub::playback_probe_service {
namespace {

using headless_playback_session_host::BoolString;
using headless_playback_session_host::DescribeProviderFallback;
using headless_playback_session_host::FormatProviderAttempts;
using headless_playback_session_host::PlaybackSessionHost;
using headless_playback_session_host::PlaybackSessionHostOptions;
using headless_playback_session_host::UsedProviderFallback;

std::map<std::string, std::string> ReadSummaryFile(agi::fs::path const& path) {
	std::map<std::string, std::string> values;
	std::ifstream in(path, std::ios::in);
	std::string line;
	while (std::getline(in, line)) {
		auto split = line.find('=');
		if (split == std::string::npos)
			continue;
		values.emplace(line.substr(0, split), line.substr(split + 1));
	}
	return values;
}

std::string GetSummaryValue(std::map<std::string, std::string> const& summary, char const* key) {
	auto it = summary.find(key);
	return it == summary.end() ? std::string() : it->second;
}

class Runner final {
	PlaybackProbeRequest request;
	std::function<void(PlaybackProbeResult)> on_done;
	PlaybackSessionHost runtime;
	std::unique_ptr<PlaybackProbeTimerHost> timer_host;
	std::vector<agi::signal::Connection> connections;
	bool probe_started = false;
	bool finished = false;
	bool playback_stop_handled = false;
	int completed_playbacks = 0;
	int performed_seeks = 0;
	int audio_timer_samples = 0;
	int seek_samples = 0;
	double total_abs_delta_ms = 0.0;
	int max_abs_delta_ms = 0;

	void AppendProbeSummary(PlaybackProbeResult const& result) const {
		std::ofstream out(runtime.TraceDir() / "summary.txt", std::ios::out | std::ios::app);
		if (!out)
			return;

		auto write_value = [&](char const* key, std::string const& value) {
			out << key << "=" << provider_selection_diagnostics::SanitizeText(value) << "\n";
		};
		auto write_bool = [&](char const* key, bool value) {
			out << key << "=" << BoolString(value) << "\n";
		};

		write_value("probe.selected.video_provider", result.selected_video_provider);
		write_value("probe.actual.video_provider", result.actual_video_provider);
		write_value("probe.actual.video_decoder", result.actual_video_decoder);
		write_bool("probe.video.provider_fallback", result.video_provider_fallback);
		write_value("probe.video.provider_fallback_reason", result.video_provider_fallback_reason);
		write_value("probe.video.provider_attempts", result.video_provider_attempts);
		write_value("probe.selected.audio_provider", result.selected_audio_provider);
		write_value("probe.actual.audio_provider_factory", result.actual_audio_provider_factory);
		write_value("probe.actual.audio_provider", result.actual_audio_provider);
		write_bool("probe.audio.provider_fallback", result.audio_provider_fallback);
		write_value("probe.audio.provider_fallback_reason", result.audio_provider_fallback_reason);
		write_value("probe.audio.provider_attempts", result.audio_provider_attempts);
		out << "probe.performed_seeks=" << result.performed_seeks << "\n";
		out << "probe.seek.max_abs_delta_ms=" << result.max_abs_delta_ms << "\n";
		out << "probe.seek.mean_abs_delta_ms=" << result.mean_abs_delta_ms << "\n";
		out << "probe.result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		write_value("probe.message", result.message);
	}

	void InstallProbeLine(int start_ms, int duration_ms) {
		auto core = runtime.GetCore();
		core.ass->Events.clear_and_dispose([](AssDialogue* line) { delete line; });

		auto* line = new AssDialogue;
		line->Row = 0;
		line->Start = start_ms;
		line->End = start_ms + duration_ms;
		line->Text = "headless playback probe";
		core.ass->Events.push_back(*line);
		core.selectionController->SetSelectionAndActive({line}, line);
		core.ass->Commit("headless playback probe", AssFile::COMMIT_NEW);
	}

	int ComputePlayableDurationMs() const {
		auto core = runtime.GetCore();
		auto* video_provider = core.project->VideoProvider();
		auto* audio_provider = core.project->AudioProvider();
		if (!video_provider)
			return 0;

		int video_duration_ms = core.videoController->TimeAtFrame(video_provider->GetFrameCount() - 1, agi::vfr::END);
		int remaining_video_ms = std::max(0, video_duration_ms - request.line_start_ms);
		if (!audio_provider)
			return std::max(0, std::min(request.duration_ms, remaining_video_ms));

		int audio_duration_ms = static_cast<int>(
			(audio_provider->GetNumSamples() * 1000 + audio_provider->GetSampleRate() - 1)
			/ audio_provider->GetSampleRate());
		int remaining_audio_ms = std::max(0, audio_duration_ms - request.line_start_ms);
		return std::max(0, std::min({request.duration_ms, remaining_video_ms, remaining_audio_ms}));
	}

	void PrintReport(PlaybackProbeResult const& result) {
		auto const summary = ReadSummaryFile(runtime.TraceDir() / "summary.txt");

		std::cout << "headless-playback-probe\n";
		std::cout << "video=" << request.video_path.string() << "\n";
		std::cout << "audio=" << request.audio_path.string() << "\n";
		std::cout << "skip_audio=" << BoolString(result.skip_audio) << "\n";
		std::cout << "line_start_ms=" << request.line_start_ms << "\n";
		std::cout << "repeat_count=" << request.repeat_count << "\n";
		std::cout << "repeat_gap_ms=" << request.repeat_gap_ms << "\n";
		std::cout << "seek_after_ms=" << (request.seek_after_ms ? std::to_string(*request.seek_after_ms) : std::string()) << "\n";
		std::cout << "seek_target_offset_ms=" << (request.seek_target_offset_ms ? std::to_string(*request.seek_target_offset_ms) : std::string()) << "\n";
		std::cout << "performed_seeks=" << result.performed_seeks << "\n";
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
		std::cout << "duration_ms=" << request.duration_ms << "\n";
		std::cout << "audio_rate_scale=" << request.audio_rate_scale << "\n";
		std::cout << "audio_quantum_ms=" << request.audio_quantum_ms << "\n";
		std::cout << "trace_dir=" << result.trace_dir.string() << "\n";
		std::cout << "seek.samples=" << result.seek_samples << "\n";
		std::cout << "seek.max_abs_delta_ms=" << result.max_abs_delta_ms << "\n";
		std::cout << "seek.mean_abs_delta_ms=" << result.mean_abs_delta_ms << "\n";
		std::cout << "audio_timer_samples=" << result.audio_timer_samples << "\n";
		std::cout << "summary.frame.request.total=" << GetSummaryValue(summary, "frame.request.total") << "\n";
		std::cout << "summary.frame.delivered.total=" << GetSummaryValue(summary, "frame.delivered.total") << "\n";
		std::cout << "summary.frame.dropped.total=" << GetSummaryValue(summary, "frame.dropped.total") << "\n";
		std::cout << "summary.audio_ui_timer_interval.count=" << GetSummaryValue(summary, "audio_ui_timer_interval.count") << "\n";
		std::cout << "summary.video_playback_tick_interval.count=" << GetSummaryValue(summary, "video_playback_tick_interval.count") << "\n";
		std::cout << "summary.audio_output_backend=" << GetSummaryValue(summary, "audio_output_backend") << "\n";
		std::cout << "result=" << (result.passed ? "PASS" : "FAIL") << "\n";
		if (!result.message.empty())
			std::cout << "message=" << result.message << "\n";
	}

	PlaybackProbeResult BuildResult(int exit_code, std::string const& message, double mean_abs_delta_ms) const {
		PlaybackProbeResult result;
		result.exit_code = exit_code;
		result.passed = exit_code == 0;
		result.skip_audio = request.skip_audio;
		result.performed_seeks = performed_seeks;
		result.audio_timer_samples = audio_timer_samples;
		result.seek_samples = seek_samples;
		result.max_abs_delta_ms = max_abs_delta_ms;
		result.mean_abs_delta_ms = mean_abs_delta_ms;
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
		return result;
	}

	void Finish(int exit_code, std::string const& message) {
		if (finished)
			return;
		finished = true;
		timer_host->StopAll();
		connections.clear();

		runtime.CloseMedia();
		runtime.ShutdownTrace();
		double mean_abs_delta_ms = seek_samples ? total_abs_delta_ms / seek_samples : 0.0;
		auto result = BuildResult(exit_code, message, mean_abs_delta_ms);
		AppendProbeSummary(result);
		PrintReport(result);
		runtime.ReleaseResources();

		if (on_done)
			on_done(std::move(result));
		delete this;
	}

	void OnAudioPlaybackPosition(int) {
		if (!probe_started || finished)
			return;
		++audio_timer_samples;
	}

	void OnPlaybackFrameAdvanced(int frame) {
		if (!probe_started || finished)
			return;

		if (request.skip_audio) {
			++seek_samples;
			return;
		}

		auto core = runtime.GetCore();
		if (!core.audioController->IsPlaying())
			return;

		++seek_samples;
		int frame_time_ms = core.videoController->TimeAtFrame(frame, agi::vfr::EXACT);
		int audio_time_ms = runtime.GetCurrentAudioPositionMs();
		int abs_delta_ms = std::abs(frame_time_ms - audio_time_ms);
		total_abs_delta_ms += abs_delta_ms;
		max_abs_delta_ms = std::max(max_abs_delta_ms, abs_delta_ms);
	}

	void OnPlaybackStopped() {
		if (finished || !probe_started)
			return;

		++completed_playbacks;
		if (completed_playbacks < request.repeat_count) {
			timer_host->StartRestartOnce(request.repeat_gap_ms);
			return;
		}

		int exit_code = 0;
		std::string message;
		if (!request.skip_audio && audio_timer_samples == 0) {
			exit_code = 2;
			message = "audio controller did not emit playback timer samples";
		}
		else if (seek_samples == 0) {
			exit_code = 3;
			message = "video controller did not emit playback seek samples";
		}
		else if (!request.skip_audio && max_abs_delta_ms > request.max_allowed_abs_delta_ms) {
			exit_code = 4;
			message = "video seek drift exceeded threshold";
		}

		Finish(exit_code, message);
	}

	void OnTimeout() {
		Finish(5, "playback probe timed out");
	}

	void OnCompletionPoll() {
		if (finished || !probe_started)
			return;

		auto core = runtime.GetCore();
		if (core.videoController->IsPlaying()) {
			playback_stop_handled = false;
			return;
		}
		if (playback_stop_handled)
			return;
		playback_stop_handled = true;
		if (!core.videoController->IsPlaying())
			OnPlaybackStopped();
	}

	void OnRestartTimer() {
		if (finished || !probe_started)
			return;

		auto core = runtime.GetCore();
		playback_stop_handled = false;
		core.videoController->PlayLine();
		if (!core.videoController->IsPlaying())
			Finish(10, "playback probe could not restart playback");
		ArmSeekTimer();
	}

	void OnSeekTimer() {
		if (finished || !probe_started || !request.seek_target_offset_ms)
			return;

		auto core = runtime.GetCore();
		++performed_seeks;
		core.videoController->JumpToTime(request.line_start_ms + *request.seek_target_offset_ms);
		if (!core.videoController->IsPlaying())
			Finish(11, "playback probe lost playback after scheduled seek");
	}

	void ArmSeekTimer() {
		timer_host->ArmSeek(request.seek_after_ms);
	}

public:
	Runner(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done))
	, runtime(PlaybackSessionHostOptions{
		this->request.video_provider,
		this->request.audio_provider,
		this->request.trace_dir,
		this->request.audio_rate_scale,
		this->request.audio_quantum_ms,
		"headless-playback-probe-%%%%%%%%",
		{
			this->request.video_track_index,
			this->request.audio_track_index,
			{},
			true
		}
	})
	, timer_host(CreatePlaybackProbeTimerHost(
		[this] { OnTimeout(); },
		[this] { OnCompletionPoll(); },
		[this] { OnRestartTimer(); },
		[this] { OnSeekTimer(); })) {
	}

	void Start() {
		int init_error_code = 0;
		std::string init_error_message;
		if (!runtime.Start(init_error_code, init_error_message)) {
			Finish(init_error_code ? init_error_code : 8,
				init_error_message.empty() ? "failed to start playback probe session" : init_error_message);
			return;
		}

		auto open_result = runtime.OpenMedia({
			request.video_path,
			request.skip_audio ? std::optional<agi::fs::path>{} : std::make_optional(request.audio_path),
			request.skip_audio
		});
		if (!open_result.opened) {
			Finish(open_result.error_code ? open_result.error_code : 8,
				open_result.error.empty() ? "failed to open project media" : open_result.error);
			return;
		}

		int playable_duration_ms = ComputePlayableDurationMs();
		if (playable_duration_ms <= 0) {
			Finish(8, "no playable duration available");
			return;
		}

		request.duration_ms = playable_duration_ms;
		InstallProbeLine(request.line_start_ms, playable_duration_ms);

		auto core = runtime.GetCore();
		connections = agi::signal::make_vector({
			core.videoController->AddPlaybackFrameAdvancedListener(&Runner::OnPlaybackFrameAdvanced, this),
			core.audioController->AddPlaybackPositionListener(&Runner::OnAudioPlaybackPosition, this),
		});

		int timeout_ms = static_cast<int>(std::ceil(playable_duration_ms / std::max(request.audio_rate_scale, 0.1))) * std::max(request.repeat_count, 1)
			+ std::max(0, request.repeat_count - 1) * request.repeat_gap_ms
			+ 3000;
		timer_host->StartTimeoutOnce(timeout_ms);
		timer_host->StartCompletionPolling(20);

		probe_started = true;
		playback_stop_handled = false;
		core.videoController->PlayLine();
		if (!core.videoController->IsPlaying()) {
			Finish(9, "video controller did not enter playback");
			return;
		}
		ArmSeekTimer();
	}
};

} // namespace

void RunAsync(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done) {
	auto* runner = new Runner(std::move(request), std::move(on_done));
	runner->Start();
}

}
