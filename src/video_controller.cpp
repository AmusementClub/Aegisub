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
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "time_range.h"
#include "async_video_provider.h"
#include "utils.h"
#include "video_controller_timer.h"
#include "video_navigation_ops.h"

#include <libaegisub/ass/time.h>

#include <wx/log.h>

VideoController::VideoController(agi::Context *c)
: context(c)
, playback_timer(CreateVideoControllerTimer([this] { OnPlayTimer(); }))
, playAudioOnStep(OPT_GET("Audio/Plays When Stepping Video"))
{
	auto core = context->GetCore();
	ui_activation.AddConnections(
		core.ass->AddCommitListener(&VideoController::OnSubtitlesCommit, this),
		core.project->AddVideoProviderListener(&VideoController::OnNewVideoProvider, this),
		core.project->AddTimecodesListener(&VideoController::OnTimecodesChanged, this),
		core.selectionController->AddActiveLineListener(&VideoController::OnActiveLineChanged, this));
}

VideoController::~VideoController() {
	ui_activation.Deactivate();
}

void VideoController::ResetPlaybackState() {
	playback_mode = PlaybackMode::None;
	playback_end_ms = 0;
	playback_uses_audio_authority = false;
	playback_seek_frame_pending = -1;
}

void VideoController::OnNewVideoProvider(AsyncVideoProvider *new_provider) {
	Stop();
	provider = new_provider;
	presented_frame_n = -1;
	ClearLatePreviewFrameAcceptance();
	ClearInspectionStepState();
	ClearRecentRenderPacketCache();
	color_matrix = provider ? provider->GetColorSpace() : "";
	ResetPlaybackState();
}

void VideoController::OnSubtitlesCommit(int type, const AssDialogue *changed) {
	if (!provider) return;
	auto core = context->GetCore();
	ClearInspectionStepState();
	ClearRecentRenderPacketCache();

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

void VideoController::OnTimecodesChanged(agi::vfr::Framerate const&) {
	ClearInspectionStepState();
	ClearRecentRenderPacketCache();
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
	if (supersede_in_flight)
		ClearLatePreviewFrameAcceptance();
	auto const frame_time = TimeAtFrame(frame_n);
	perf_trace::ObserveFrameRequest(frame_n, frame_time, false);
	provider->RequestFrame(frame_n, frame_time, supersede_in_flight);
}

void VideoController::RequestFrameImmediate() {
	auto core = context->GetCore();
	ClearInspectionStepState();
	core.ass->Properties.video_position = frame_n;
	ClearLatePreviewFrameAcceptance();
	auto const frame_time = TimeAtFrame(frame_n);
	perf_trace::ObserveFrameRequest(frame_n, frame_time, true);

	try {
		provider->CancelPendingFrameRequests();

		// Synchronous callers favor deterministic display over latest-only coalescing.
		int const requested_frame = frame_n;
		auto packet = provider->GetRenderPacket(frame_n, frame_time);
		perf_trace::ObserveFrameResult(frame_n, frame_time, true, true);
		DeliverFrameReady(std::move(packet), frame_time);
		if (presented_frame_n != requested_frame)
			NotifyFramePresented(requested_frame);
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

	ClearInspectionStepState();
	if (!supersede_in_flight && !accept_late_preview_frames)
		provider->CancelPendingFrameRequests();

	frame_n = mid(0, target_frame, provider->GetFrameCount() - 1);
	accept_late_preview_frames = !supersede_in_flight;
	if (accept_late_preview_frames)
		acceptable_late_preview_frames.insert(frame_n);
	else
		acceptable_late_preview_frames.clear();
	if (trace)
		perf_trace::TraceSeek(frame_n, false);
	RequestFrame(supersede_in_flight);
	Seek(frame_n);
}

void VideoController::ClearLatePreviewFrameAcceptance() {
	accept_late_preview_frames = false;
	acceptable_late_preview_frames.clear();
}

void VideoController::ClearInspectionStepState() {
	inspection_step_in_flight = false;
	inspection_step_frame = -1;
	inspection_step_anchor_frame = -1;
	has_pending_inspection_step = false;
	pending_inspection_step_frame = -1;
	pending_inspection_step_play_audio = false;
	pending_inspection_step_delta = 0;
}

int VideoController::GetInspectionStepAnchorFrame() const {
	if ((inspection_step_in_flight || has_pending_inspection_step) && inspection_step_anchor_frame >= 0)
		return inspection_step_anchor_frame;
	return frame_n;
}

void VideoController::HandleInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta) {
	if (!provider)
		return;

	if (inspection_step_in_flight) {
		if (target == inspection_step_anchor_frame)
			return;

		inspection_step_anchor_frame = target;
		if (target == inspection_step_frame) {
			has_pending_inspection_step = false;
			pending_inspection_step_frame = -1;
			pending_inspection_step_play_audio = false;
			pending_inspection_step_delta = 0;
		}
		else {
			has_pending_inspection_step = true;
			pending_inspection_step_frame = target;
			pending_inspection_step_play_audio = play_audio;
			pending_inspection_step_delta = delta;
		}
		return;
	}

	if (target == frame_n)
		return;

	RequestInspectionStepTarget(target, immediate_request, play_audio, delta);
}

void VideoController::RequestInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta) {
	frame_n = target;
	inspection_step_in_flight = true;
	inspection_step_frame = frame_n;
	inspection_step_anchor_frame = frame_n;
	has_pending_inspection_step = false;
	pending_inspection_step_frame = -1;
	pending_inspection_step_play_audio = false;
	pending_inspection_step_delta = 0;
	ClearLatePreviewFrameAcceptance();
	perf_trace::TraceSeek(frame_n, false);
	bool const delivered_from_cache = immediate_request && TrySeekAndDeliverRecentRenderPacket(frame_n);
	if (!delivered_from_cache) {
		RequestFrame();
		Seek(frame_n);
	}

	PlayInspectionStepAudio(play_audio, delta);
}

void VideoController::PlayInspectionStepAudio(bool play_audio, int delta) {
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

void VideoController::StepFrames(int delta, bool play_audio_on_inspection) {
	if (!provider || IsPlaying())
		return;

	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0)
		return;

	int const target = mid(0, GetInspectionStepAnchorFrame() + delta, frame_count - 1);
	HandleInspectionStepTarget(target, true, play_audio_on_inspection, delta);
}

void VideoController::NavigateToFrame(int target_frame) {
	if (!provider || IsPlaying())
		return;

	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0)
		return;

	int const target = mid(0, target_frame, frame_count - 1);
	HandleInspectionStepTarget(target, true, false, target - GetInspectionStepAnchorFrame());
}

void VideoController::NavigateToKeyframe(std::vector<int> const& keyframes, int direction) {
	if (!provider || IsPlaying())
		return;

	int const anchor_frame = GetInspectionStepAnchorFrame();
	int const target = direction < 0
		? aegisub::video_navigation_ops::ComputePreviousKeyframe(keyframes, anchor_frame)
		: aegisub::video_navigation_ops::ComputeNextKeyframe(keyframes, anchor_frame, provider->GetFrameCount() - 1);
	HandleInspectionStepTarget(target, true, false, direction);
}

void VideoController::JumpToFrame(int n) {
	if (!provider) return;
	ClearInspectionStepState();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	ClearLatePreviewFrameAcceptance();
	playback_seek_frame_pending = was_playing ? frame_n : -1;
	perf_trace::TraceSeek(frame_n, was_playing);
	bool const delivered_from_cache = !was_playing && TrySeekAndDeliverRecentRenderPacket(frame_n);
	if (!delivered_from_cache) {
		RequestFrame();
		Seek(frame_n);
	}

	if (was_playing) {
		if (!PreparePlayback(resume_mode, frame_n, resume_end_ms)) {
			Stop();
			return;
		}
		playback_start_time = std::chrono::steady_clock::now();
		perf_trace::ResetVideoPlaybackInterval();
	}
}

void VideoController::PreviewToFrame(int n) {
	if (!provider) return;
	ClearInspectionStepState();

	bool const already_accepting_late_preview = accept_late_preview_frames;
	if (!already_accepting_late_preview)
		ClearLatePreviewFrameAcceptance();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;
	if (was_playing) {
		Stop();
		provider->CancelPendingFrameRequests();
	}
	else if (!already_accepting_late_preview) {
		provider->CancelPendingFrameRequests();
	}

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	accept_late_preview_frames = true;
	acceptable_late_preview_frames.insert(frame_n);
	perf_trace::TraceSeek(frame_n, was_playing);
	RequestFrame(false);
	Seek(frame_n);

	if (was_playing && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
}

void VideoController::PreviewToFrameLatest(int n) {
	if (!provider) return;

	ClearInspectionStepState();
	ClearLatePreviewFrameAcceptance();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;
	if (was_playing)
		Stop();

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	perf_trace::TraceSeek(frame_n, was_playing);
	RequestFrame(true);
	Seek(frame_n);

	if (was_playing && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
}

void VideoController::JumpToTime(int ms, agi::vfr::Time end) {
	if (!provider) return;

	JumpToFrame(FrameAtTime(ms, end));
}

void VideoController::NavigateByFrames(int delta) {
	StepFrames(delta, false);
}

void VideoController::StepSingleFrame(int delta) {
	StepFrames(delta, playAudioOnStep->GetBool());
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
	if (mode == PlaybackMode::ToEnd && presented_frame_n >= 0)
		frame_n = presented_frame_n;

	ClearInspectionStepState();
	playback_seek_frame_pending = -1;
	if (provider)
		provider->CancelPendingFrameRequests();
	ClearLatePreviewFrameAcceptance();

	if (!PreparePlayback(mode, frame_n, range_end_ms))
		return;

	RequestFrame();
	StartPlaybackTimer();
}

void VideoController::Play() {
	if (IsPlaying()) {
		Stop();
		return;
	}

	StartPlayback(PlaybackMode::ToEnd);
}

void VideoController::PlayLine() {
	Stop();
	auto core = context->GetCore();

	AssDialogue *curline = core.selectionController->GetActiveLine();
	if (!curline) return;

	// Round-trip conversion to convert start to exact
	int startFrame = FrameAtTime(curline->Start, agi::vfr::START);
	if (provider)
		provider->CancelPendingFrameRequests();
	if (!PreparePlayback(PlaybackMode::LineRange, startFrame, curline->End))
		return;

	JumpToFrame(startFrame);
	StartPlaybackTimer();
}

void VideoController::Stop() {
	ClearInspectionStepState();
	ClearLatePreviewFrameAcceptance();
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

void VideoController::OnPlayTimer() {
	using namespace std::chrono;
	auto core = context->GetCore();
	if (playback_seek_frame_pending >= 0)
		return;

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
	playback_seek_frame_pending = -1;
	ClearInspectionStepState();
	ClearRecentRenderPacketCache();
	wxLogError(
		wxS("Failed seeking video. The video file may be corrupt or incomplete.\n"
		    "Error message reported: %s"),
		to_wx(message));
}

void VideoController::HandleSubtitlesError(std::string const& message) {
	playback_seek_frame_pending = -1;
	ClearInspectionStepState();
	ClearRecentRenderPacketCache();
	wxLogError(
		wxS("Failed rendering subtitles. Error message reported: %s"),
		to_wx(message));
}

void VideoController::RememberRecentRenderPacket(VideoRenderPacket const& packet) {
	if (packet.frame_number < 0)
		return;

	for (auto it = recent_render_packets.begin(); it != recent_render_packets.end(); ++it) {
		if (it->frame_number == packet.frame_number) {
			recent_render_packets.erase(it);
			break;
		}
	}

	recent_render_packets.push_front(packet);
	constexpr size_t max_recent_packets = 8;
	while (recent_render_packets.size() > max_recent_packets)
		recent_render_packets.pop_back();
}

void VideoController::ClearRecentRenderPacketCache() {
	recent_render_packets.clear();
}

bool VideoController::TrySeekAndDeliverRecentRenderPacket(int frame) {
	for (auto it = recent_render_packets.begin(); it != recent_render_packets.end(); ++it) {
		if (it->frame_number != frame)
			continue;

		perf_trace::ObserveVideoRenderPacketCacheLookup(frame, true, "recent_render_packet");
		auto packet = *it;
		recent_render_packets.erase(it);
		recent_render_packets.push_front(packet);
		double const packet_time = packet.time;
		context->GetCore().ass->Properties.video_position = frame;
		Seek(frame);
		DeliverFrameReady(std::move(packet), packet_time);
		return true;
	}

	perf_trace::ObserveVideoRenderPacketCacheLookup(frame, false, "recent_render_packet");
	return false;
}

void VideoController::DeliverFrameReady(VideoRenderPacket packet, double time) {
	if (packet.frame_number != frame_n) {
		if (!accept_late_preview_frames || !acceptable_late_preview_frames.count(packet.frame_number))
			return;
		acceptable_late_preview_frames.erase(packet.frame_number);
		frame_n = packet.frame_number;
		context->GetCore().ass->Properties.video_position = frame_n;
		Seek(frame_n);
	}
	if (playback_seek_frame_pending == packet.frame_number)
		playback_seek_frame_pending = -1;
	else if (playback_seek_frame_pending >= 0)
		return;

	RememberRecentRenderPacket(packet);
	FrameReady(packet, time);
}

void VideoController::NotifyFramePresented(int frame_number) {
	presented_frame_n = frame_number;
	FramePresented(frame_number);
	if (inspection_step_in_flight && frame_number == inspection_step_frame) {
		inspection_step_in_flight = false;
		inspection_step_frame = -1;
		RequestPendingInspectionStepTarget();
	}
}

void VideoController::RequestPendingInspectionStepTarget() {
	if (!has_pending_inspection_step) {
		inspection_step_anchor_frame = frame_n;
		return;
	}

	int const target = pending_inspection_step_frame;
	bool const play_audio = pending_inspection_step_play_audio;
	int const delta = pending_inspection_step_delta;
	has_pending_inspection_step = false;
	pending_inspection_step_frame = -1;
	pending_inspection_step_play_audio = false;
	pending_inspection_step_delta = 0;

	if (!provider || IsPlaying() || target == frame_n) {
		inspection_step_anchor_frame = frame_n;
		return;
	}

	RequestInspectionStepTarget(target, true, play_audio, delta);
}

void VideoController::InvalidateRenderPacketCache() {
	ClearInspectionStepState();
	ClearRecentRenderPacketCache();
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
