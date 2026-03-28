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

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "audio_provider_factory.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "project_open_service.h"
#include "selection_controller.h"
#include "status_sink.h"
#include "ui_services.h"
#include "version.h"
#include "video_controller.h"
#include "video_provider_manager.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegisub::playback_probe_service {
namespace {

using Clock = std::chrono::steady_clock;
using ProviderSelectionReport = provider_selection_diagnostics::SelectionReport;

agi::fs::path UniqueProbeTraceDir() {
	auto root = config::path->Decode("?temp");
	agi::fs::CreateDirectory(root);
	return agi::fs::UniquePath(root / "headless-playback-probe-%%%%%%%%");
}

class ConsoleStatusSink final : public agi::StatusSink {
public:
	void ShowStatus(std::string const& message, int) override {
		std::cout << "[status] " << message << "\n";
	}
};

class ConsoleNotificationSink final : public agi::NotificationSink {
	void Print(char const* kind, std::string const& title, std::string const& message) {
		std::cerr << "[" << kind << "] " << title;
		if (!message.empty())
			std::cerr << ": " << message;
		std::cerr << "\n";
	}

public:
	void ShowInfo(std::string const& title, std::string const& message) override {
		Print("info", title, message);
	}

	void ShowError(std::string const& title, std::string const& message) override {
		Print("error", title, message);
	}

	void ShowWarning(std::string const& title, std::string const& message) override {
		Print("warning", title, message);
	}
};

class FakeAudioClockState final {
	double rate_scale = 1.0;
	int quantum_ms = 0;
	int sample_rate = 1;
	int64_t start_sample = 0;
	int64_t end_sample = 0;
	int64_t current_sample = 0;
	bool playing = false;
	Clock::time_point real_start = Clock::now();

	int64_t SamplesFromMilliseconds(double ms) const {
		return sample_rate > 0
			? static_cast<int64_t>(std::llround(ms * sample_rate / 1000.0))
			: 0;
	}

	double QuantizeMilliseconds(double ms) const {
		if (quantum_ms <= 0 || ms <= 0.0)
			return ms;
		return std::floor(ms / quantum_ms) * quantum_ms;
	}

	void ObserveSnapshot(char const* reason) const {
		perf_trace::AudioOutputSnapshot snapshot;
		snapshot.backend_name = "headless-fake";
		snapshot.reason = reason;
		snapshot.queued_buffers = 0;
		snapshot.queued_ms = 0.0;
		snapshot.end_of_stream = !playing && current_sample >= end_sample;
		perf_trace::ObserveAudioOutputSnapshot(snapshot);
	}

public:
	FakeAudioClockState(double rate_scale, int quantum_ms)
	: rate_scale(rate_scale)
	, quantum_ms(quantum_ms) {
	}

	void Begin(agi::AudioProvider* provider, int64_t start, int64_t count) {
		sample_rate = provider ? provider->GetSampleRate() : 1;
		start_sample = start;
		end_sample = std::max<int64_t>(start, start + std::max<int64_t>(count, 0));
		current_sample = start;
		playing = true;
		real_start = Clock::now();
		ObserveSnapshot("play");
	}

	int64_t CurrentSample() {
		if (!playing)
			return current_sample;

		double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - real_start).count();
		elapsed_ms = QuantizeMilliseconds(elapsed_ms * rate_scale);
		current_sample = std::min(end_sample, start_sample + SamplesFromMilliseconds(elapsed_ms));
		if (current_sample >= end_sample)
			playing = false;
		ObserveSnapshot("tick");
		return current_sample;
	}

	void Stop() {
		CurrentSample();
		playing = false;
		ObserveSnapshot("stop");
	}

	bool IsPlaying() {
		CurrentSample();
		return playing;
	}

	void SetEndPosition(int64_t position) {
		end_sample = std::max(start_sample, position);
	}

	int64_t GetEndPosition() const {
		return end_sample;
	}

	int64_t GetCurrentPosition() {
		return CurrentSample();
	}

	int GetCurrentPositionMs() {
		return sample_rate > 0
			? static_cast<int>(GetCurrentPosition() * 1000 / sample_rate)
			: 0;
	}
};

class HeadlessFakeAudioPlayer final : public AudioPlayer {
	std::shared_ptr<FakeAudioClockState> state;

public:
	HeadlessFakeAudioPlayer(agi::AudioProvider* provider, std::shared_ptr<FakeAudioClockState> state)
	: AudioPlayer(provider)
	, state(std::move(state)) {
	}

	void Play(int64_t start, int64_t count) override {
		state->Begin(provider, start, count);
	}

	void Stop() override {
		state->Stop();
	}

	bool IsPlaying() override {
		return state->IsPlaying();
	}

	void SetVolume(double) override {
	}

	int64_t GetEndPosition() override {
		return state->GetEndPosition();
	}

	int64_t GetCurrentPosition() override {
		return state->GetCurrentPosition();
	}

	void SetEndPosition(int64_t pos) override {
		state->SetEndPosition(pos);
	}
};

class HeadlessFakeAudioPlayerFactoryService final : public agi::AudioPlayerFactoryService {
	std::shared_ptr<FakeAudioClockState> state;

public:
	explicit HeadlessFakeAudioPlayerFactoryService(std::shared_ptr<FakeAudioClockState> state)
	: state(std::move(state)) {
	}

	std::unique_ptr<AudioPlayer> CreateAudioPlayer(agi::AudioProvider* provider) override {
		return std::make_unique<HeadlessFakeAudioPlayer>(provider, state);
	}
};

class ScopedTemporaryMru final {
	agi::MRUManager* temporary_mru = nullptr;
	agi::MRUManager* previous_mru = nullptr;

public:
	explicit ScopedTemporaryMru(agi::fs::path const& path) {
		previous_mru = config::mru;
		temporary_mru = new agi::MRUManager(path, GET_DEFAULT_CONFIG(default_mru), config::opt);
		config::mru = temporary_mru;
	}

	~ScopedTemporaryMru() {
		config::mru = previous_mru;
		delete temporary_mru;
	}
};

class ScopedTemporaryStringOption final {
	std::string option_name;
	std::string previous_value;
	bool active = false;

public:
	ScopedTemporaryStringOption(char const* option_name, std::optional<std::string> const& temporary_value)
	: option_name(option_name) {
		if (!temporary_value)
			return;

		auto* option = OPT_SET(this->option_name);
		previous_value = option->GetString();
		option->SetString(*temporary_value);
		active = true;
	}

	~ScopedTemporaryStringOption() {
		if (!active)
			return;
		OPT_SET(option_name)->SetString(previous_value);
	}
};

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

std::string BoolString(bool value) {
	return value ? "true" : "false";
}

bool UsedProviderFallback(ProviderSelectionReport const& report) {
	return provider_selection_diagnostics::UsedFallback(report);
}

std::string FormatProviderAttempts(ProviderSelectionReport const& report) {
	return provider_selection_diagnostics::FormatAttempts(report);
}

std::string DescribeProviderFallback(ProviderSelectionReport const& report) {
	return provider_selection_diagnostics::DescribeFallbackReason(report);
}

class PlaybackProbeRuntime final {
	std::unique_ptr<agi::Context> context = std::make_unique<agi::Context>();
	std::shared_ptr<FakeAudioClockState> fake_audio_state;
	std::shared_ptr<HeadlessFakeAudioPlayerFactoryService> fake_audio_service;
	std::shared_ptr<ConsoleNotificationSink> notification_sink = std::make_shared<ConsoleNotificationSink>();
	std::shared_ptr<ConsoleStatusSink> status_sink = std::make_shared<ConsoleStatusSink>();
	std::optional<ScopedTemporaryMru> temporary_mru;
	std::optional<ScopedTemporaryStringOption> temporary_video_provider;
	std::optional<ScopedTemporaryStringOption> temporary_audio_provider;
	agi::fs::path trace_dir;
	bool perf_trace_initialized = false;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	ProviderSelectionReport video_provider_report;
	ProviderSelectionReport audio_provider_report;
	std::string actual_video_provider;
	std::string actual_video_decoder;
	std::string actual_audio_provider_factory;
	std::string actual_audio_provider;

public:
	explicit PlaybackProbeRuntime(PlaybackProbeRequest const& request)
	: fake_audio_state(std::make_shared<FakeAudioClockState>(request.audio_rate_scale, request.audio_quantum_ms))
	, fake_audio_service(std::make_shared<HeadlessFakeAudioPlayerFactoryService>(fake_audio_state)) {
	}

	agi::ContextCoreSession GetCore() {
		return context->GetCore();
	}

	agi::ConstContextCoreSession GetCore() const {
		return static_cast<agi::Context const&>(*context).GetCore();
	}

	int GetCurrentAudioPositionMs() {
		return fake_audio_state->GetCurrentPositionMs();
	}

	agi::fs::path const& TraceDir() const {
		return trace_dir;
	}

	std::string const& SelectedVideoProvider() const {
		return selected_video_provider;
	}

	std::string const& SelectedAudioProvider() const {
		return selected_audio_provider;
	}

	std::string const& ActualVideoProvider() const {
		return actual_video_provider;
	}

	std::string const& ActualVideoDecoder() const {
		return actual_video_decoder;
	}

	std::string const& ActualAudioProviderFactory() const {
		return actual_audio_provider_factory;
	}

	std::string const& ActualAudioProvider() const {
		return actual_audio_provider;
	}

	ProviderSelectionReport const& VideoProviderReport() const {
		return video_provider_report;
	}

	ProviderSelectionReport const& AudioProviderReport() const {
		return audio_provider_report;
	}

	bool StartSession(PlaybackProbeRequest const& request, int& error_code, std::string& error_message) {
		trace_dir = request.trace_dir.value_or(UniqueProbeTraceDir());
		agi::fs::CreateDirectory(trace_dir.parent_path());
		agi::fs::CreateDirectory(trace_dir);
		temporary_mru.emplace(trace_dir / "probe_mru.json");
		temporary_video_provider.emplace("Video/Provider", request.video_provider);
		if (!request.skip_audio)
			temporary_audio_provider.emplace("Audio/Provider", request.audio_provider);

		perf_trace::InitializeAt(trace_dir, GetAegisubLongVersionString(), "audio,video,ops");
		perf_trace_initialized = true;

		auto core = GetCore();
		core.statusSink = status_sink;
		core.notificationSink = notification_sink;
		core.audioPlayerFactoryService = fake_audio_service;

		core.ass->LoadDefault(false);
		OPT_SET("Video/Open Audio")->SetBool(false);
		selected_video_provider = OPT_GET("Video/Provider")->GetString();
		selected_audio_provider = request.skip_audio ? std::string() : OPT_GET("Audio/Provider")->GetString();

		ClearLastVideoProviderSelectionReport();
		if (!request.skip_audio)
			ClearLastAudioProviderSelectionReport();

		auto open_result = project_open_service::Open(*context, {
			request.video_path,
			request.skip_audio ? std::optional<agi::fs::path>{} : std::make_optional(request.audio_path),
			request.skip_audio
		});

		video_provider_report = GetLastVideoProviderSelectionReport();
		actual_video_provider = video_provider_report.selected_provider;
		if (!request.skip_audio) {
			audio_provider_report = GetLastAudioProviderSelectionReport();
			actual_audio_provider_factory = audio_provider_report.selected_provider;
		}
		if (!open_result.opened) {
			error_code = open_result.error_code ? open_result.error_code : 8;
			error_message = open_result.error.empty() ? "failed to open project media" : open_result.error;
			return false;
		}
		actual_video_decoder = open_result.media.video_decoder_name;
		actual_audio_provider = open_result.media.audio_provider_name;
		return true;
	}

	void CloseMedia() {
		if (!context)
			return;

		auto core = GetCore();
		if (core.videoController->IsPlaying())
			core.videoController->Stop();
		core.project->CloseAudio();
		core.project->CloseVideo();
	}

	void ShutdownTrace() {
		if (!perf_trace_initialized)
			return;
		perf_trace::Shutdown();
		perf_trace_initialized = false;
	}

	void ReleaseResources() {
		context.reset();
		temporary_audio_provider.reset();
		temporary_video_provider.reset();
		temporary_mru.reset();
	}
};

class PlaybackProbeTimerHost final : public wxEvtHandler {
	std::function<void()> on_timeout;
	std::function<void()> on_completion_poll;
	std::function<void()> on_restart_timer;
	std::function<void()> on_seek_timer;
	wxTimer timeout_timer{this};
	wxTimer completion_timer{this};
	wxTimer restart_timer{this};
	wxTimer seek_timer{this};

	void HandleTimeout(wxTimerEvent&) {
		if (on_timeout)
			on_timeout();
	}

	void HandleCompletionPoll(wxTimerEvent&) {
		if (on_completion_poll)
			on_completion_poll();
	}

	void HandleRestartTimer(wxTimerEvent&) {
		if (on_restart_timer)
			on_restart_timer();
	}

	void HandleSeekTimer(wxTimerEvent&) {
		if (on_seek_timer)
			on_seek_timer();
	}

public:
	PlaybackProbeTimerHost(
		std::function<void()> on_timeout,
		std::function<void()> on_completion_poll,
		std::function<void()> on_restart_timer,
		std::function<void()> on_seek_timer)
	: on_timeout(std::move(on_timeout))
	, on_completion_poll(std::move(on_completion_poll))
	, on_restart_timer(std::move(on_restart_timer))
	, on_seek_timer(std::move(on_seek_timer)) {
		Bind(wxEVT_TIMER, &PlaybackProbeTimerHost::HandleTimeout, this, timeout_timer.GetId());
		Bind(wxEVT_TIMER, &PlaybackProbeTimerHost::HandleCompletionPoll, this, completion_timer.GetId());
		Bind(wxEVT_TIMER, &PlaybackProbeTimerHost::HandleRestartTimer, this, restart_timer.GetId());
		Bind(wxEVT_TIMER, &PlaybackProbeTimerHost::HandleSeekTimer, this, seek_timer.GetId());
	}

	void StopAll() {
		timeout_timer.Stop();
		completion_timer.Stop();
		restart_timer.Stop();
		seek_timer.Stop();
	}

	void StartTimeoutOnce(int timeout_ms) {
		timeout_timer.Start(timeout_ms, true);
	}

	void StartCompletionPolling(int interval_ms) {
		completion_timer.Start(interval_ms);
	}

	void StartRestartOnce(int delay_ms) {
		restart_timer.StartOnce(delay_ms);
	}

	void ArmSeek(std::optional<int> delay_ms) {
		seek_timer.Stop();
		if (delay_ms)
			seek_timer.StartOnce(*delay_ms);
	}
};

class Runner final {
	PlaybackProbeRequest request;
	std::function<void(PlaybackProbeResult)> on_done;
	PlaybackProbeRuntime runtime;
	PlaybackProbeTimerHost timer_host;
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
		timer_host.StopAll();
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
			timer_host.StartRestartOnce(request.repeat_gap_ms);
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
		timer_host.ArmSeek(request.seek_after_ms);
	}

public:
	Runner(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done))
	, runtime(this->request)
	, timer_host(
		[this] { OnTimeout(); },
		[this] { OnCompletionPoll(); },
		[this] { OnRestartTimer(); },
		[this] { OnSeekTimer(); }) {
	}

	void Start() {
		int init_error_code = 0;
		std::string init_error_message;
		if (!runtime.StartSession(request, init_error_code, init_error_message)) {
			Finish(init_error_code ? init_error_code : 8,
				init_error_message.empty() ? "failed to open project media" : init_error_message);
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
		timer_host.StartTimeoutOnce(timeout_ms);
		timer_host.StartCompletionPolling(20);

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
