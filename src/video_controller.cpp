// Copyright (c) 2005-2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "video_controller.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_controller.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "playback_transport_policy.h"
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "time_range.h"
#include "async_video_provider.h"
#include "utils.h"
#include "video_controller_timer.h"

#include <libaegisub/ass/time.h>

#include <cstdlib>
#include <cctype>
#include <limits>
#include <wx/log.h>

namespace {

std::optional<int> ReadEnvInt(char const* name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return std::nullopt;

	char* end = nullptr;
	long parsed = std::strtol(value, &end, 10);
	if (end == value)
		return std::nullopt;
	if (parsed < std::numeric_limits<int>::min())
		return std::numeric_limits<int>::min();
	if (parsed > std::numeric_limits<int>::max())
		return std::numeric_limits<int>::max();
	return static_cast<int>(parsed);
}

bool ReadEnvFlagDefaultOn(char const* name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return true;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

}

VideoController::VideoController(agi::Context *c)
: context(c)
, playback_timer(CreateVideoControllerTimer([this] { OnPlayTimer(); }))
, step_preview_timer(CreateVideoControllerTimer([this] { OnStepPreviewTimer(); }))
, step_release_timer(CreateVideoControllerTimer([this] { OnStepReleaseTimer(); }))
, playAudioOnStep(OPT_GET("Audio/Plays When Stepping Video"))
{
	step_preview_enabled = ReadEnvFlagDefaultOn("AEGISUB_VIDEO_STEP_PREVIEW");

	if (auto ms = ReadEnvInt("AEGISUB_VIDEO_STEP_PREVIEW_INTERVAL_MS"))
		step_preview_interval = std::chrono::milliseconds(std::max(1, *ms));
	step_preview_interval_backward = std::max(step_preview_interval_backward, step_preview_interval);
	if (auto ms = ReadEnvInt("AEGISUB_VIDEO_STEP_PREVIEW_INTERVAL_BACKWARD_MS"))
		step_preview_interval_backward = std::chrono::milliseconds(std::max(1, *ms));
	if (auto ms = ReadEnvInt("AEGISUB_VIDEO_STEP_REPEAT_BURST_WINDOW_MS"))
		step_repeat_burst_window = std::chrono::milliseconds(std::max(0, *ms));
	if (auto n = ReadEnvInt("AEGISUB_VIDEO_STEP_REPEAT_BURST_THRESHOLD"))
		step_repeat_burst_threshold = std::max(1, *n);
	if (auto ms = ReadEnvInt("AEGISUB_VIDEO_STEP_REPEAT_RELEASE_DELAY_MS"))
		step_repeat_release_delay = std::chrono::milliseconds(std::max(0, *ms));

	perf_trace::TraceVideoStepPreviewConfig(
		step_preview_enabled,
		static_cast<int>(step_preview_interval.count()),
		static_cast<int>(step_preview_interval_backward.count()),
		static_cast<int>(step_repeat_burst_window.count()),
		step_repeat_burst_threshold,
		static_cast<int>(step_repeat_release_delay.count()));

	auto core = context->GetCore();
	ui_activation.AddConnections(
		core.ass->AddCommitListener(&VideoController::OnSubtitlesCommit, this),
		core.project->AddVideoProviderListener(&VideoController::OnNewVideoProvider, this),
		core.selectionController->AddActiveLineListener(&VideoController::OnActiveLineChanged, this));
}

VideoController::~VideoController() {
	ui_activation.Deactivate();
}

void VideoController::ResetPlaybackState() {
	playback_mode = PlaybackMode::None;
	playback_end_ms = 0;
	playback_uses_audio_authority = false;
}

void VideoController::OnNewVideoProvider(AsyncVideoProvider *new_provider) {
	Stop();
	provider = new_provider;
	color_matrix = provider ? provider->GetColorSpace() : "";
	ResetPlaybackState();
}

void VideoController::OnSubtitlesCommit(int type, const AssDialogue *changed) {
	if (!provider) return;
	auto core = context->GetCore();

	if ((type & AssFile::COMMIT_SCRIPTINFO) || type == AssFile::COMMIT_NEW) {
		auto new_matrix = core.ass->GetScriptInfo("YCbCr Matrix");
		if (!new_matrix.empty() && new_matrix != color_matrix) {
			color_matrix = new_matrix;
			provider->SetColorSpace(new_matrix);
		}
	}

	if (!changed)
		provider->LoadSubtitles(core.ass.get());
	else
		provider->UpdateSubtitles(core.ass.get(), changed);
}

void VideoController::OnActiveLineChanged(AssDialogue *line) {
	if (line && provider && OPT_GET("Video/Subtitle Sync")->GetBool()) {
		Stop();
		JumpToTime(line->Start);
	}
}

void VideoController::RequestFrame() {
	RequestFrame(true);
}

void VideoController::RequestFrame(bool supersede_in_flight) {
	auto core = context->GetCore();
	core.ass->Properties.video_position = frame_n;
	auto const frame_time = TimeAtFrame(frame_n);
	perf_trace::ObserveFrameRequest(frame_n, frame_time, false);
	provider->RequestFrame(frame_n, frame_time, supersede_in_flight);
}

void VideoController::RequestFrameImmediate() {
	auto core = context->GetCore();
	core.ass->Properties.video_position = frame_n;
	auto const frame_time = TimeAtFrame(frame_n);
	perf_trace::ObserveFrameRequest(frame_n, frame_time, true);

	try {
		provider->CancelPendingFrameRequests();

		// Frame stepping favors deterministic per-step display over latest-only coalescing.
		auto packet = provider->GetRenderPacket(frame_n, frame_time);
		perf_trace::ObserveFrameResult(frame_n, frame_time, true, true);
		DeliverFrameReady(std::move(packet), frame_time);
	}
	catch (AsyncVideoProviderVideoError const& err) {
		HandleVideoError(err.GetMessage());
	}
	catch (AsyncVideoProviderSubtitlesError const& err) {
		HandleSubtitlesError(err.GetMessage());
	}
}

void VideoController::RequestFramePreview(int target_frame, bool trace, bool supersede_in_flight) {
	if (!provider)
		return;

	frame_n = mid(0, target_frame, provider->GetFrameCount() - 1);
	if (trace)
		perf_trace::TraceSeek(frame_n, false);
	RequestFrame(supersede_in_flight);
	Seek(frame_n);
}

void VideoController::CancelStepPreviewSession() {
	if (step_preview_active)
		perf_trace::TraceVideoStepPreviewCancel(frame_n);

	if (step_transport_policy) {
		step_transport_policy->Apply({
			PlaybackTransportPolicy::InputKind::CancelPreview,
			PlaybackTransportPolicy::InteractionKind::StepRepeat,
			0,
			false
		}, std::chrono::steady_clock::now());
	}

	ResetStepPreviewSessionState();
}

void VideoController::ResetStepPreviewSessionState() {
	step_preview_active = false;
	step_preview_target_frame = -1;
	has_step_last_input = false;
	step_burst_count = 0;
	step_transport_policy_direction = 0;
	if (step_preview_timer && step_preview_timer->IsRunning())
		step_preview_timer->Stop();
	if (step_release_timer && step_release_timer->IsRunning())
		step_release_timer->Stop();
}

PlaybackTransportPolicy &VideoController::EnsureStepTransportPolicy() {
	if (!step_transport_policy)
		step_transport_policy = std::make_unique<PlaybackTransportPolicy>(step_preview_interval);
	return *step_transport_policy;
}

PlaybackTransportPolicy &VideoController::EnsureStepPreviewTransportPolicy(int direction) {
	auto const preview_interval = direction < 0 ? step_preview_interval_backward : step_preview_interval;
	if (!step_transport_policy || step_transport_policy_direction != direction) {
		step_transport_policy = std::make_unique<PlaybackTransportPolicy>(preview_interval);
		step_transport_policy_direction = direction;
	}
	return *step_transport_policy;
}

void VideoController::ScheduleStepPreviewTimer(std::chrono::steady_clock::time_point now) {
	if (!step_transport_policy)
		return;

	auto const next = step_transport_policy->NextPreviewTime();
	if (!next) {
		if (step_preview_timer->IsRunning())
			step_preview_timer->Stop();
		return;
	}

	auto const remaining = *next - now;
	auto const remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
	int const delay_ms = remaining_ms > 1 ? static_cast<int>(remaining_ms) : 1;
	step_preview_timer->StartOnce(delay_ms);
}

void VideoController::HandleInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta) {
	if (!provider || target == frame_n)
		return;

	frame_n = target;
	perf_trace::TraceSeek(frame_n, false);
	if (immediate_request)
		RequestFrameImmediate();
	else
		RequestFrame();
	Seek(frame_n);

	if (!play_audio)
		return;

	auto core = context->GetCore();
	if (delta > 0) {
		core.audioController->PlayRange(TimeRange(TimeAtFrame(frame_n - 1), TimeAtFrame(frame_n)));
	}
	else if (delta < 0) {
		core.audioController->PlayRange(TimeRange(TimeAtFrame(frame_n), TimeAtFrame(frame_n + 1)));
	}
}

void VideoController::HandleStepTransportOutputs(
	const std::vector<PlaybackTransportPolicy::Output> &outputs,
	bool immediate_inspection,
	bool play_audio_on_inspection,
	int delta)
{
	for (auto const& output : outputs) {
		switch (output.kind) {
			case PlaybackTransportPolicy::OutputKind::Preview:
				RequestFramePreview(output.target, true, false);
				break;

			case PlaybackTransportPolicy::OutputKind::Commit:
				perf_trace::TraceVideoStepPreviewRelease(step_preview_target_frame);
				RequestFramePreview(output.target, true, true);
				ResetStepPreviewSessionState();
				break;

			case PlaybackTransportPolicy::OutputKind::Inspection:
				HandleInspectionStepTarget(output.target, immediate_inspection, play_audio_on_inspection, delta);
				break;

			case PlaybackTransportPolicy::OutputKind::StartPlayback:
			case PlaybackTransportPolicy::OutputKind::StopPlayback:
				break;
		}
	}
}

void VideoController::StepFrames(int delta, bool immediate_inspection, bool play_audio_on_inspection) {
	if (!provider || IsPlaying())
		return;

	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0)
		return;

	auto const now = std::chrono::steady_clock::now();
	if (has_step_last_input && now - step_last_input_time <= step_repeat_burst_window)
		++step_burst_count;
	else
		step_burst_count = 1;
	step_last_input_time = now;
	has_step_last_input = true;

	if (step_preview_enabled && (step_preview_active || step_burst_count >= step_repeat_burst_threshold)) {
		int const direction = delta < 0 ? -1 : 1;
		auto &transport = EnsureStepPreviewTransportPolicy(direction);

		bool const first_preview_input = !step_preview_active;
		if (first_preview_input) {
			step_preview_target_frame = frame_n;
			step_preview_active = true;
			perf_trace::TraceVideoStepPreviewBegin(frame_n, delta, step_burst_count, step_repeat_burst_threshold);
		}

		int const next_target = mid(0, step_preview_target_frame + delta, frame_count - 1);
		if (next_target != step_preview_target_frame) {
			step_preview_target_frame = next_target;
			auto outputs = transport.Apply({
				PlaybackTransportPolicy::InputKind::PreviewMotion,
				PlaybackTransportPolicy::InteractionKind::StepRepeat,
				step_preview_target_frame,
				first_preview_input
			}, now);
			HandleStepTransportOutputs(outputs, immediate_inspection, play_audio_on_inspection, delta);
		}

		ScheduleStepPreviewTimer(now);
		step_release_timer->StartOnce(static_cast<int>(step_repeat_release_delay.count()));
		return;
	}

	int const target = mid(0, frame_n + delta, frame_count - 1);
	if (target == frame_n)
		return;

	auto outputs = EnsureStepTransportPolicy().Apply({
		PlaybackTransportPolicy::InputKind::InspectionStep,
		PlaybackTransportPolicy::InteractionKind::StepRepeat,
		target,
		false
	}, now);
	HandleStepTransportOutputs(outputs, immediate_inspection, play_audio_on_inspection, delta);
}

void VideoController::JumpToFrame(int n) {
	if (!provider) return;

	CancelStepPreviewSession();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;
	if (was_playing)
		Stop();

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	perf_trace::TraceSeek(frame_n, was_playing);
	RequestFrame();
	Seek(frame_n);

	if (was_playing && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
}

void VideoController::PreviewToFrame(int n) {
	if (!provider) return;

	CancelStepPreviewSession();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;
	if (was_playing) {
		Stop();
		provider->CancelPendingFrameRequests();
	}

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	perf_trace::TraceSeek(frame_n, was_playing);
	RequestFrame(false);
	Seek(frame_n);

	if (was_playing && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
}

void VideoController::JumpToTime(int ms, agi::vfr::Time end) {
	JumpToFrame(FrameAtTime(ms, end));
}

void VideoController::NavigateByFrames(int delta) {
	StepFrames(delta, false, false);
}

void VideoController::StepSingleFrame(int delta) {
	StepFrames(delta, true, playAudioOnStep->GetBool());
}

void VideoController::NextFrame() {
	StepSingleFrame(1);
}

void VideoController::PrevFrame() {
	StepSingleFrame(-1);
}

bool VideoController::PreparePlayback(PlaybackMode mode, int start_frame, int range_end_ms) {
	if (!provider || mode == PlaybackMode::None)
		return false;

	auto core = context->GetCore();
	start_ms = TimeAtFrame(start_frame);
	playback_mode = mode;
	playback_end_ms = range_end_ms;
	if (mode == PlaybackMode::LineRange) {
		end_frame = FrameAtTime(playback_end_ms, agi::vfr::END) + 1;
		if (start_ms >= playback_end_ms) {
			ResetPlaybackState();
			return false;
		}
		core.audioController->PlayRange(TimeRange(start_ms, playback_end_ms));
	}
	else {
		end_frame = provider->GetFrameCount();
		core.audioController->PlayToEnd(start_ms);
	}
	playback_uses_audio_authority = core.audioController->IsPlaying();
	return true;
}

void VideoController::StartPlaybackTimer() {
	playback_start_time = std::chrono::steady_clock::now();
	perf_trace::ResetVideoPlaybackInterval();
	perf_trace::TracePlayStart(frame_n, start_ms);
	playback_timer->Start(10);
}

void VideoController::StartPlayback(PlaybackMode mode, int range_end_ms) {
	if (!PreparePlayback(mode, frame_n, range_end_ms))
		return;

	StartPlaybackTimer();
}

void VideoController::Play() {
	CancelStepPreviewSession();

	if (IsPlaying()) {
		Stop();
		return;
	}

	StartPlayback(PlaybackMode::ToEnd);
}

void VideoController::PlayLine() {
	CancelStepPreviewSession();

	Stop();
	auto core = context->GetCore();

	AssDialogue *curline = core.selectionController->GetActiveLine();
	if (!curline) return;

	// Round-trip conversion to convert start to exact
	int startFrame = FrameAtTime(curline->Start, agi::vfr::START);
	if (!PreparePlayback(PlaybackMode::LineRange, startFrame, curline->End))
		return;

	JumpToFrame(startFrame);
	StartPlaybackTimer();
}

void VideoController::Stop() {
	if (IsPlaying()) {
		perf_trace::TracePlayStop(frame_n);
		perf_trace::ResetVideoPlaybackInterval();
		playback_timer->Stop();
		playback_uses_audio_authority = false;
		auto core = context->GetCore();
		core.audioController->Stop();
	}
	ResetPlaybackState();
}

bool VideoController::IsPlaying() const {
	return playback_timer && playback_timer->IsRunning();
}

void VideoController::OnStepPreviewTimer() {
	if (!step_preview_active || !step_transport_policy || !provider)
		return;

	auto const now = std::chrono::steady_clock::now();
	auto outputs = step_transport_policy->Apply({
		PlaybackTransportPolicy::InputKind::Timer,
		PlaybackTransportPolicy::InteractionKind::StepRepeat,
		0,
		false
	}, now);
	HandleStepTransportOutputs(outputs, false, false, 0);
	ScheduleStepPreviewTimer(now);
}

void VideoController::OnStepReleaseTimer() {
	if (!step_preview_active || !step_transport_policy || !provider)
		return;

	auto const now = std::chrono::steady_clock::now();
	auto outputs = step_transport_policy->Apply({
		PlaybackTransportPolicy::InputKind::PreviewRelease,
		PlaybackTransportPolicy::InteractionKind::StepRepeat,
		step_preview_target_frame,
		false
	}, now);
	HandleStepTransportOutputs(outputs, false, false, 0);
}

void VideoController::OnPlayTimer() {
	using namespace std::chrono;
	auto core = context->GetCore();

	int authority_time_ms = start_ms + duration_cast<milliseconds>(steady_clock::now() - playback_start_time).count();
	if (playback_uses_audio_authority) {
		if (!core.audioController->IsPlaying()) {
			Stop();
			return;
		}
		authority_time_ms = core.audioController->GetPlaybackPosition();
	}

	int next_frame = FrameAtTime(authority_time_ms);
	perf_trace::ObserveVideoPlaybackTick(next_frame);

	bool const reached_end = next_frame >= end_frame;
	if (reached_end)
		next_frame = end_frame - 1;

	if (next_frame != frame_n) {
		frame_n = next_frame;
		RequestFrame();
		Seek(frame_n);
		PlaybackFrameAdvanced(frame_n);
	}

	if (reached_end)
		Stop();
}

double VideoController::GetARFromType(AspectRatio type) const {
	switch (type) {
		case AspectRatio::Default:    return (double)provider->GetWidth()/provider->GetHeight();
		case AspectRatio::Fullscreen: return 4.0/3.0;
		case AspectRatio::Widescreen: return 16.0/9.0;
		case AspectRatio::Cinematic:  return 2.35;
        default: throw agi::InternalError("Bad AR type");
	}
}

void VideoController::SetAspectRatio(double value) {
	ar_type = AspectRatio::Custom;
	ar_value = mid(.5, value, 5.);
	auto core = context->GetCore();
	core.ass->Properties.ar_mode = (int)ar_type;
	core.ass->Properties.ar_value = ar_value;
	ARChange(ar_type, ar_value);
}

void VideoController::SetAspectRatio(AspectRatio type) {
	ar_value = mid(.5, GetARFromType(type), 5.);
	ar_type = type;
	auto core = context->GetCore();
	core.ass->Properties.ar_mode = (int)ar_type;
	core.ass->Properties.ar_value = ar_value;
	ARChange(ar_type, ar_value);
}

int VideoController::TimeAtFrame(int frame, agi::vfr::Time type) const {
	auto core = context->GetCore();
	return core.project->Timecodes().TimeAtFrame(frame, type);
}

int VideoController::FrameAtTime(int time, agi::vfr::Time type) const {
	auto core = context->GetCore();
	return core.project->Timecodes().FrameAtTime(time, type);
}

void VideoController::HandleVideoError(std::string const& message) {
	wxLogError(
		wxS("Failed seeking video. The video file may be corrupt or incomplete.\n"
		    "Error message reported: %s"),
		to_wx(message));
}

void VideoController::HandleSubtitlesError(std::string const& message) {
	wxLogError(
		wxS("Failed rendering subtitles. Error message reported: %s"),
		to_wx(message));
}

void VideoController::DeliverFrameReady(VideoRenderPacket packet, double time) {
	FrameReady(packet, time);
}

void VideoController::NotifyFramePresented(int frame_number) {
	if (presented_frame_n == frame_number)
		return;
	presented_frame_n = frame_number;
	FramePresented(frame_number);
}

AsyncVideoProviderEventSink VideoController::CreateAsyncVideoProviderEventSink() {
	return CreateAsyncVideoProviderMainThreadSink(
		GetAsyncUiLifetime(),
		{
			[this](VideoRenderPacket packet, double time) {
				DeliverFrameReady(std::move(packet), time);
			},
			[this](std::string const& message) {
				HandleVideoError(message);
			},
			[this](std::string const& message) {
				HandleSubtitlesError(message);
			}
		});
}
