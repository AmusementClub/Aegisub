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

#include "headless_playback_probe.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "status_sink.h"
#include "ui_services.h"
#include "version.h"
#include "video_controller.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <wx/app.h>
#include <wx/arrstr.h>
#include <wx/string.h>
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

namespace headless_playback_probe {
namespace {

using Clock = std::chrono::steady_clock;

std::string ToNarrow(wxString const& value) {
	return value.ToStdString(wxConvUTF8);
}

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

	void Begin(agi::AudioProvider *provider, int64_t start, int64_t count) {
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
	HeadlessFakeAudioPlayer(agi::AudioProvider *provider, std::shared_ptr<FakeAudioClockState> state)
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

	std::unique_ptr<AudioPlayer> CreateAudioPlayer(agi::AudioProvider *provider) override {
		return std::make_unique<HeadlessFakeAudioPlayer>(provider, state);
	}
};

class ScopedTemporaryMru final {
	agi::MRUManager *temporary_mru = nullptr;
	agi::MRUManager *previous_mru = nullptr;

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

		auto *option = OPT_SET(this->option_name);
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

std::optional<int> ParseInt(std::string const& text) {
	try {
		size_t consumed = 0;
		int value = std::stoi(text, &consumed);
		if (consumed != text.size())
			return std::nullopt;
		return value;
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<double> ParseDouble(std::string const& text) {
	try {
		size_t consumed = 0;
		double value = std::stod(text, &consumed);
		if (consumed != text.size())
			return std::nullopt;
		return value;
	}
	catch (...) {
		return std::nullopt;
	}
}

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

class Runner final : public wxEvtHandler {
	Options options;
	std::function<void(int)> on_done;
	std::unique_ptr<agi::Context> context = std::make_unique<agi::Context>();
	std::vector<agi::signal::Connection> connections;
	std::shared_ptr<FakeAudioClockState> fake_audio_state;
	std::shared_ptr<HeadlessFakeAudioPlayerFactoryService> fake_audio_service;
	std::shared_ptr<ConsoleNotificationSink> notification_sink = std::make_shared<ConsoleNotificationSink>();
	std::shared_ptr<ConsoleStatusSink> status_sink = std::make_shared<ConsoleStatusSink>();
	std::optional<ScopedTemporaryMru> temporary_mru;
	std::optional<ScopedTemporaryStringOption> temporary_video_provider;
	std::optional<ScopedTemporaryStringOption> temporary_audio_provider;
	wxTimer timeout_timer{this};
	agi::fs::path trace_dir;
	bool probe_started = false;
	bool finished = false;
	int audio_timer_samples = 0;
	int seek_samples = 0;
	double total_abs_delta_ms = 0.0;
	int max_abs_delta_ms = 0;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	std::string actual_video_decoder;
	std::string actual_audio_provider;

	void InstallProbeLine(int duration_ms) {
		auto core = context->GetCore();
		core.ass->Events.clear_and_dispose([](AssDialogue *line) { delete line; });

		auto *line = new AssDialogue;
		line->Row = 0;
		line->Start = 0;
		line->End = duration_ms;
		line->Text = "headless playback probe";
		core.ass->Events.push_back(*line);
		core.selectionController->SetSelectionAndActive({line}, line);
		core.ass->Commit("headless playback probe", AssFile::COMMIT_NEW);
	}

	int ComputePlayableDurationMs() const {
		auto core = context->GetCore();
		auto *video_provider = core.project->VideoProvider();
		auto *audio_provider = core.project->AudioProvider();
		if (!video_provider || !audio_provider)
			return 0;

		int video_duration_ms = core.videoController->TimeAtFrame(video_provider->GetFrameCount() - 1, agi::vfr::END);
		int audio_duration_ms = static_cast<int>(
			(audio_provider->GetNumSamples() * 1000 + audio_provider->GetSampleRate() - 1)
			/ audio_provider->GetSampleRate());
		return std::max(0, std::min({options.duration_ms, video_duration_ms, audio_duration_ms}));
	}

	void PrintReport(int exit_code, std::string const& message) {
		auto const summary = ReadSummaryFile(trace_dir / "summary.txt");
		double mean_abs_delta_ms = seek_samples ? total_abs_delta_ms / seek_samples : 0.0;

		std::cout << "headless-playback-probe\n";
		std::cout << "video=" << options.video_path.string() << "\n";
		std::cout << "audio=" << options.audio_path.string() << "\n";
		std::cout << "selected.video_provider=" << selected_video_provider << "\n";
		std::cout << "selected.audio_provider=" << selected_audio_provider << "\n";
		std::cout << "actual.video_decoder=" << actual_video_decoder << "\n";
		std::cout << "actual.audio_provider=" << actual_audio_provider << "\n";
		std::cout << "duration_ms=" << options.duration_ms << "\n";
		std::cout << "audio_rate_scale=" << options.audio_rate_scale << "\n";
		std::cout << "audio_quantum_ms=" << options.audio_quantum_ms << "\n";
		std::cout << "trace_dir=" << trace_dir.string() << "\n";
		std::cout << "seek.samples=" << seek_samples << "\n";
		std::cout << "seek.max_abs_delta_ms=" << max_abs_delta_ms << "\n";
		std::cout << "seek.mean_abs_delta_ms=" << mean_abs_delta_ms << "\n";
		std::cout << "audio_timer_samples=" << audio_timer_samples << "\n";
		std::cout << "summary.frame.request.total=" << GetSummaryValue(summary, "frame.request.total") << "\n";
		std::cout << "summary.frame.delivered.total=" << GetSummaryValue(summary, "frame.delivered.total") << "\n";
		std::cout << "summary.frame.dropped.total=" << GetSummaryValue(summary, "frame.dropped.total") << "\n";
		std::cout << "summary.audio_ui_timer_interval.count=" << GetSummaryValue(summary, "audio_ui_timer_interval.count") << "\n";
		std::cout << "summary.video_playback_tick_interval.count=" << GetSummaryValue(summary, "video_playback_tick_interval.count") << "\n";
		std::cout << "summary.audio_output_backend=" << GetSummaryValue(summary, "audio_output_backend") << "\n";
		std::cout << "result=" << (exit_code == 0 ? "PASS" : "FAIL") << "\n";
		if (!message.empty())
			std::cout << "message=" << message << "\n";
	}

	void Finish(int exit_code, std::string const& message) {
		if (finished)
			return;
		finished = true;
		timeout_timer.Stop();
		connections.clear();

		if (context) {
			auto core = context->GetCore();
			if (core.videoController->IsPlaying())
				core.videoController->Stop();
			core.project->CloseAudio();
			core.project->CloseVideo();
		}
		temporary_audio_provider.reset();
		temporary_video_provider.reset();

		perf_trace::Shutdown();
		PrintReport(exit_code, message);
		context.reset();
		temporary_mru.reset();

		if (on_done)
			on_done(exit_code);
		delete this;
	}

	void OnAudioPlaybackPosition(int) {
		if (!probe_started || finished)
			return;
		++audio_timer_samples;
	}

	void OnVideoSeek(int frame) {
		if (!probe_started || finished)
			return;

		auto core = context->GetCore();
		int frame_time_ms = core.videoController->TimeAtFrame(frame, agi::vfr::EXACT);
		int audio_time_ms = fake_audio_state->GetCurrentPositionMs();
		int abs_delta_ms = std::abs(frame_time_ms - audio_time_ms);
		++seek_samples;
		total_abs_delta_ms += abs_delta_ms;
		max_abs_delta_ms = std::max(max_abs_delta_ms, abs_delta_ms);
	}

	void OnPlaybackStopped() {
		if (finished || !probe_started)
			return;

		int exit_code = 0;
		std::string message;
		if (audio_timer_samples == 0) {
			exit_code = 2;
			message = "audio controller did not emit playback timer samples";
		}
		else if (seek_samples == 0) {
			exit_code = 3;
			message = "video controller did not emit playback seek samples";
		}
		else if (max_abs_delta_ms > options.max_allowed_abs_delta_ms) {
			exit_code = 4;
			message = "video seek drift exceeded threshold";
		}

		Finish(exit_code, message);
	}

	void OnTimeout(wxTimerEvent&) {
		Finish(5, "playback probe timed out");
	}

public:
	Runner(Options options, std::function<void(int)> on_done)
	: options(std::move(options))
	, on_done(std::move(on_done))
	, fake_audio_state(std::make_shared<FakeAudioClockState>(this->options.audio_rate_scale, this->options.audio_quantum_ms))
	, fake_audio_service(std::make_shared<HeadlessFakeAudioPlayerFactoryService>(fake_audio_state)) {
	}

	void Start() {
		Bind(wxEVT_TIMER, &Runner::OnTimeout, this);

		trace_dir = options.trace_dir.value_or(UniqueProbeTraceDir());
		agi::fs::CreateDirectory(trace_dir.parent_path());
		temporary_mru.emplace(trace_dir / "probe_mru.json");
		temporary_video_provider.emplace("Video/Provider", options.video_provider);
		temporary_audio_provider.emplace("Audio/Provider", options.audio_provider);

		perf_trace::InitializeAt(trace_dir, GetAegisubLongVersionString(), "audio,video,ops");

		auto core = context->GetCore();
		core.statusSink = status_sink;
		core.notificationSink = notification_sink;
		core.audioPlayerFactoryService = fake_audio_service;

		core.ass->LoadDefault(false);
		OPT_SET("Video/Open Audio")->SetBool(false);
		selected_video_provider = OPT_GET("Video/Provider")->GetString();
		selected_audio_provider = OPT_GET("Audio/Provider")->GetString();

		core.project->LoadVideo(options.video_path);
		if (!core.project->VideoProvider()) {
			Finish(6, "failed to load video");
			return;
		}
		actual_video_decoder = core.project->VideoProvider()->GetDecoderName();

		core.project->LoadAudio(options.audio_path);
		if (!core.project->AudioProvider()) {
			Finish(7, "failed to load audio");
			return;
		}
		actual_audio_provider = core.project->AudioProvider()->GetMemoryStats().provider_name;

		int playable_duration_ms = ComputePlayableDurationMs();
		if (playable_duration_ms <= 0) {
			Finish(8, "no playable duration available");
			return;
		}

		options.duration_ms = playable_duration_ms;
		InstallProbeLine(playable_duration_ms);

		connections = agi::signal::make_vector({
			core.videoController->AddSeekListener(&Runner::OnVideoSeek, this),
			core.audioController->AddPlaybackPositionListener(&Runner::OnAudioPlaybackPosition, this),
			core.audioController->AddPlaybackStopListener(&Runner::OnPlaybackStopped, this),
		});

		int timeout_ms = static_cast<int>(std::ceil(playable_duration_ms / std::max(options.audio_rate_scale, 0.1))) + 3000;
		timeout_timer.Start(timeout_ms, true);

		probe_started = true;
		core.videoController->PlayLine();
		if (!core.videoController->IsPlaying()) {
			Finish(9, "video controller did not enter playback");
			return;
		}
	}
};

bool IsRequested(wxArrayString const& args) {
	for (size_t i = 1; i < args.size(); ++i) {
		if (ToNarrow(args[i]) == "--headless-playback-probe")
			return true;
	}
	return false;
}

} // namespace

ParseResult Parse(wxArrayString const& args) {
	ParseResult result;
	result.requested = IsRequested(args);
	if (!result.requested)
		return result;

	Options options;
	bool have_video = false;

	auto require_value = [&](size_t& index, char const* flag) -> std::optional<std::string> {
		if (index + 1 >= args.size()) {
			result.error = std::string(flag) + " requires a value\n" + Usage();
			return std::nullopt;
		}
		++index;
		return ToNarrow(args[index]);
	};

	for (size_t i = 1; i < args.size(); ++i) {
		auto arg = ToNarrow(args[i]);
		if (arg == "--headless-playback-probe")
			continue;
		if (arg == "--probe-video") {
			auto value = require_value(i, "--probe-video");
			if (!value)
				return result;
			options.video_path = *value;
			have_video = true;
			continue;
		}
		if (arg == "--probe-audio") {
			auto value = require_value(i, "--probe-audio");
			if (!value)
				return result;
			options.audio_path = *value;
			continue;
		}
		if (arg == "--probe-video-provider") {
			auto value = require_value(i, "--probe-video-provider");
			if (!value)
				return result;
			options.video_provider = *value;
			continue;
		}
		if (arg == "--probe-audio-provider") {
			auto value = require_value(i, "--probe-audio-provider");
			if (!value)
				return result;
			options.audio_provider = *value;
			continue;
		}
		if (arg == "--probe-duration-ms") {
			auto value = require_value(i, "--probe-duration-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed <= 0) {
				result.error = "--probe-duration-ms must be a positive integer\n" + Usage();
				return result;
			}
			options.duration_ms = *parsed;
			continue;
		}
		if (arg == "--probe-audio-rate-scale") {
			auto value = require_value(i, "--probe-audio-rate-scale");
			if (!value)
				return result;
			auto parsed = ParseDouble(*value);
			if (!parsed || *parsed <= 0.0) {
				result.error = "--probe-audio-rate-scale must be a positive number\n" + Usage();
				return result;
			}
			options.audio_rate_scale = *parsed;
			continue;
		}
		if (arg == "--probe-audio-quantum-ms") {
			auto value = require_value(i, "--probe-audio-quantum-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed < 0) {
				result.error = "--probe-audio-quantum-ms must be a non-negative integer\n" + Usage();
				return result;
			}
			options.audio_quantum_ms = *parsed;
			continue;
		}
		if (arg == "--probe-max-abs-delta-ms") {
			auto value = require_value(i, "--probe-max-abs-delta-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed <= 0) {
				result.error = "--probe-max-abs-delta-ms must be a positive integer\n" + Usage();
				return result;
			}
			options.max_allowed_abs_delta_ms = *parsed;
			continue;
		}
		if (arg == "--probe-trace-dir") {
			auto value = require_value(i, "--probe-trace-dir");
			if (!value)
				return result;
			options.trace_dir = agi::fs::path(*value);
			continue;
		}

		result.error = "unrecognized headless playback probe argument: " + arg + "\n" + Usage();
		return result;
	}

	if (!have_video) {
		result.error = "--headless-playback-probe requires --probe-video\n" + Usage();
		return result;
	}
	if (options.audio_path.empty())
		options.audio_path = options.video_path;

	result.options = std::move(options);
	return result;
}

void RunAsync(Options options, std::function<void(int)> on_done) {
	auto *runner = new Runner(std::move(options), std::move(on_done));
	runner->Start();
}

std::string Usage() {
	return
		"Usage: Aegisub.exe --headless-playback-probe --probe-video <path> "
		"[--probe-audio <path>] [--probe-video-provider <name>] [--probe-audio-provider <name>] "
		"[--probe-duration-ms <ms>] "
		"[--probe-audio-rate-scale <scale>] [--probe-audio-quantum-ms <ms>] "
		"[--probe-max-abs-delta-ms <ms>] [--probe-trace-dir <path>]";
}

} // namespace headless_playback_probe
