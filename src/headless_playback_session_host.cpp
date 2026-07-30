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

#include "headless_playback_session_host.h"

#include "ass_file.h"
#include "ass_file_app.h"
#include "audio_controller.h"
#include "audio_provider_factory.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "status_sink.h"
#include "track_choice.h"
#include "ui_services.h"
#include "version.h"
#include "video_controller.h"
#include "video_provider_manager.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

namespace aegisub::headless_playback_session_host {
namespace {

using Clock = std::chrono::steady_clock;
using ProviderSelectionReport = provider_selection_diagnostics::SelectionReport;

agi::fs::path UniqueTraceDir(std::string const& pattern) {
	auto root = config::path->Decode("?temp");
	agi::fs::CreateDirectory(root);
	return agi::fs::UniquePath(root / pattern);
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

class HeadlessTrackChoiceInteractionSink final : public agi::SingleChoiceInteractionSink {
	HeadlessTrackChoiceConfig config;

	std::optional<int> ResolveConfiguredChoice(std::string const& request_id) const {
		if (request_id == "track_choice.video")
			return config.video_track_index;
		if (request_id == "track_choice.audio")
			return config.audio_track_index;
		if (request_id == "track_choice.subtitle")
			return config.subtitle_track_index;
		return std::nullopt;
	}

public:
	explicit HeadlessTrackChoiceInteractionSink(HeadlessTrackChoiceConfig config)
	: config(std::move(config)) {
	}

	std::optional<int> RequestSingleChoice(agi::SingleChoiceInteractionRequest const& request) override {
		if (!agi::util::strings::starts_with(request.request_id, "track_choice."))
			return std::nullopt;

		auto configured = ResolveConfiguredChoice(request.request_id);
		if (configured) {
			if (*configured >= 0 && *configured < static_cast<int>(request.choices.size()))
				return configured;
			return std::nullopt;
		}

		if (!config.default_to_first_track || request.choices.empty())
			return std::nullopt;

		auto default_choice = request.default_choice;
		if (default_choice < 0 || default_choice >= static_cast<int>(request.choices.size()))
			default_choice = 0;
		return default_choice;
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
		temporary_mru = new agi::MRUManager(path, libresrc_getconfig(default_mru, default_mru_size), config::opt);
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
		option->SetString(provider_selection_diagnostics::CanonicalizeProviderName(*temporary_value));
		active = true;
	}

	~ScopedTemporaryStringOption() {
		if (!active)
			return;
		OPT_SET(option_name)->SetString(previous_value);
	}
};

}

class PlaybackSessionHost::Impl final {
public:
	explicit Impl(PlaybackSessionHostOptions options)
	: options(std::move(options))
	, context(std::make_unique<agi::Context>())
	, fake_audio_state(std::make_shared<FakeAudioClockState>(this->options.audio_rate_scale, this->options.audio_quantum_ms))
	, fake_audio_service(std::make_shared<HeadlessFakeAudioPlayerFactoryService>(fake_audio_state))
	, notification_sink(std::make_shared<ConsoleNotificationSink>())
	, status_sink(std::make_shared<ConsoleStatusSink>())
	, track_choice_sink(std::make_shared<HeadlessTrackChoiceInteractionSink>(this->options.track_choice_config)) {
	}

	PlaybackSessionHostOptions options;
	std::unique_ptr<agi::Context> context;
	std::shared_ptr<FakeAudioClockState> fake_audio_state;
	std::shared_ptr<HeadlessFakeAudioPlayerFactoryService> fake_audio_service;
	std::shared_ptr<ConsoleNotificationSink> notification_sink;
	std::shared_ptr<ConsoleStatusSink> status_sink;
	std::shared_ptr<HeadlessTrackChoiceInteractionSink> track_choice_sink;
	std::optional<ScopedTemporaryMru> temporary_mru;
	std::optional<ScopedTemporaryStringOption> temporary_video_provider;
	std::optional<ScopedTemporaryStringOption> temporary_audio_provider;
	std::optional<project_open_service::PlaybackOpenOptions> last_open_options;
	agi::fs::path trace_dir;
	bool started = false;
	bool perf_trace_initialized = false;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	ProviderSelectionReport video_provider_report;
	ProviderSelectionReport audio_provider_report;
	std::string actual_video_provider;
	std::string actual_video_decoder;
	std::string actual_audio_provider_factory;
	std::string actual_audio_provider;

	agi::ContextCoreSession GetCore() {
		return context->GetCore();
	}

	agi::ContextCoreSession GetCore() const {
		return const_cast<agi::Context&>(*context).GetCore();
	}

	bool Start(int& error_code, std::string& error_message) {
		if (!context) {
			error_code = 2;
			error_message = "playback session context is unavailable";
			return false;
		}
		if (started)
			return true;

		auto pattern = options.trace_dir_pattern.empty()
			? std::string("headless-playback-session-%%%%%%%%")
			: options.trace_dir_pattern;
		trace_dir = options.trace_dir.value_or(UniqueTraceDir(pattern));
		agi::fs::CreateDirectory(trace_dir.parent_path());
		agi::fs::CreateDirectory(trace_dir);
		temporary_mru.emplace(trace_dir / "session_mru.json");
		temporary_video_provider.emplace("Video/Provider", options.video_provider);
		temporary_audio_provider.emplace("Audio/Provider", options.audio_provider);

		perf_trace::InitializeAt(trace_dir, GetAegisubLongVersionString(), "audio,video,ops");
		perf_trace_initialized = true;

		auto core = GetCore();
		core.statusSink = status_sink;
		core.notificationSink = notification_sink;
		core.singleChoiceInteractionSink = track_choice_sink;
		core.audioPlayerFactoryService = fake_audio_service;
		LoadDefaultAssFileWithAppOptions(*core.ass, false);
		OPT_SET("Video/Open Audio")->SetBool(false);

		started = true;
		return true;
	}

	project_open_service::ProjectOpenResult OpenMedia(project_open_service::PlaybackOpenOptions const& options) {
		project_open_service::ProjectOpenResult result;
		if (!started) {
			result.error_code = 2;
			result.error = "playback session host is not started";
			return result;
		}
		if (!context) {
			result.error_code = 2;
			result.error = "playback session context is unavailable";
			return result;
		}

		last_open_options = options;
		CloseMedia();

		selected_video_provider = provider_selection_diagnostics::CanonicalizeProviderName(OPT_GET("Video/Provider")->GetString());
		selected_audio_provider = options.skip_audio
			? std::string()
			: provider_selection_diagnostics::CanonicalizeProviderName(OPT_GET("Audio/Provider")->GetString());
		video_provider_report = {};
		audio_provider_report = {};
		actual_video_provider.clear();
		actual_video_decoder.clear();
		actual_audio_provider_factory.clear();
		actual_audio_provider.clear();

		ClearLastVideoProviderSelectionReport();
		if (!options.skip_audio)
			ClearLastAudioProviderSelectionReport();

		result = project_open_service::Open(GetCore(), options);
		video_provider_report = GetLastVideoProviderSelectionReport();
		actual_video_provider = video_provider_report.selected_provider;
		if (!options.skip_audio) {
			audio_provider_report = GetLastAudioProviderSelectionReport();
			actual_audio_provider_factory = audio_provider_report.selected_provider;
		}
		actual_video_decoder = result.media.video_decoder_name;
		actual_audio_provider = result.media.audio_provider_name;
		return result;
	}

	project_open_service::ProjectOpenResult ReopenMedia() {
		project_open_service::ProjectOpenResult result;
		if (!last_open_options) {
			result.error_code = 3;
			result.error = "playback session host has no previous open request";
			return result;
		}
		return OpenMedia(*last_open_options);
	}

	void CloseMedia() {
		if (!context)
			return;

		auto core = GetCore();
		if (core.videoController && core.videoController->IsPlaying())
			core.videoController->Stop();
		if (core.audioController && core.audioController->IsPlaying())
			core.audioController->Stop();
		if (core.project) {
			core.project->CloseAudio();
			core.project->CloseVideo();
		}
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
		last_open_options.reset();
		started = false;
		selected_video_provider.clear();
		selected_audio_provider.clear();
		actual_video_provider.clear();
		actual_video_decoder.clear();
		actual_audio_provider_factory.clear();
		actual_audio_provider.clear();
		video_provider_report = {};
		audio_provider_report = {};
	}
};

PlaybackSessionHost::PlaybackSessionHost(PlaybackSessionHostOptions options)
: impl(std::make_unique<Impl>(std::move(options))) {
}

PlaybackSessionHost::~PlaybackSessionHost() {
	if (!impl)
		return;
	impl->CloseMedia();
	impl->ShutdownTrace();
	impl->ReleaseResources();
}

agi::ContextCoreSession PlaybackSessionHost::GetCore() {
	return impl->GetCore();
}

agi::ContextCoreSession PlaybackSessionHost::GetCore() const {
	return static_cast<Impl const&>(*impl).GetCore();
}

agi::Context *PlaybackSessionHost::GetContext() {
	return impl ? impl->context.get() : nullptr;
}

agi::Context const* PlaybackSessionHost::GetContext() const {
	return impl ? impl->context.get() : nullptr;
}

bool PlaybackSessionHost::Start(int& error_code, std::string& error_message) {
	return impl->Start(error_code, error_message);
}

project_open_service::ProjectOpenResult PlaybackSessionHost::OpenMedia(project_open_service::PlaybackOpenOptions const& options) {
	return impl->OpenMedia(options);
}

project_open_service::ProjectOpenResult PlaybackSessionHost::ReopenMedia() {
	return impl->ReopenMedia();
}

void PlaybackSessionHost::CloseMedia() {
	impl->CloseMedia();
}

void PlaybackSessionHost::ShutdownTrace() {
	impl->ShutdownTrace();
}

void PlaybackSessionHost::ReleaseResources() {
	impl->ReleaseResources();
}

bool PlaybackSessionHost::HasLastOpenOptions() const {
	return impl->last_open_options.has_value();
}

int PlaybackSessionHost::GetCurrentAudioPositionMs() {
	return impl->fake_audio_state->GetCurrentPositionMs();
}

agi::fs::path const& PlaybackSessionHost::TraceDir() const {
	return impl->trace_dir;
}

std::string const& PlaybackSessionHost::SelectedVideoProvider() const {
	return impl->selected_video_provider;
}

std::string const& PlaybackSessionHost::SelectedAudioProvider() const {
	return impl->selected_audio_provider;
}

std::string const& PlaybackSessionHost::ActualVideoProvider() const {
	return impl->actual_video_provider;
}

std::string const& PlaybackSessionHost::ActualVideoDecoder() const {
	return impl->actual_video_decoder;
}

std::string const& PlaybackSessionHost::ActualAudioProviderFactory() const {
	return impl->actual_audio_provider_factory;
}

std::string const& PlaybackSessionHost::ActualAudioProvider() const {
	return impl->actual_audio_provider;
}

ProviderSelectionReport const& PlaybackSessionHost::VideoProviderReport() const {
	return impl->video_provider_report;
}

ProviderSelectionReport const& PlaybackSessionHost::AudioProviderReport() const {
	return impl->audio_provider_report;
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

}
