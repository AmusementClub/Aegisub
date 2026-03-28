// Copyright (c) 2009-2010, Niels Martin Hansen
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

#include "audio_controller.h"

#include "audio_controller_power_host.h"
#include "audio_controller_timer_host.h"
#include "audio_timing.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "ui_services.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>

namespace {
constexpr int64_t kPlaybackAheadMs = 8000;
constexpr int64_t kPlaybackBehindMs = 2000;
constexpr int kAudioUiTimerRequestedMs = 20;
}

AudioController::AudioController(agi::Context *context)
: context(context)
, playback_timer(CreateAudioControllerTimerHost([this] { OnPlaybackTimer(); }))
, power_host(CreateAudioControllerPowerHost(
	[this] { HandleComputerSuspending(); },
	[this] { HandleComputerResuming(); }))
, provider_connection(context->GetCore().project->AddAudioProviderListener(&AudioController::OnAudioProvider, this))
{
	OPT_SUB("Audio/Player", &AudioController::OnAudioPlayerChanged, this);
}

AudioController::~AudioController()
{
	Stop();
}

void AudioController::OnPlaybackTimer()
{
	if (!player) return;

	int64_t pos = player->GetCurrentPosition();
	if (!player->IsPlaying() ||
		(playback_mode != PM_ToEnd && pos >= player->GetEndPosition()+200))
	{
		// The +200 is to allow the player to end the sound output cleanly,
		// otherwise a popping artifact can sometimes be heard.
		Stop();
	}
	else
	{
		if (provider) {
			provider->SetPlaybackWindow(
				pos,
				SamplesFromMilliseconds(kPlaybackAheadMs),
				SamplesFromMilliseconds(kPlaybackBehindMs));
		}
		auto const position_ms = MillisecondsFromSamples(pos);
		perf_trace::ObserveAudioUiTimerPosition(position_ms);
		AnnouncePlaybackPosition(position_ms);
	}
}

void AudioController::HandleComputerSuspending()
{
	Stop();
	player.reset();
}

void AudioController::HandleComputerResuming()
{
	OnAudioPlayerChanged();
}

void AudioController::OnAudioPlayerChanged()
{
	if (!provider) return;

	Stop();
	player.reset();
	auto core = context->GetCore();

	try
	{
		if (core.audioPlayerFactoryService)
			player = core.audioPlayerFactoryService->CreateAudioPlayer(provider);
	}
	catch (...)
	{
		/// @todo This really shouldn't be just swallowing all audio player open errors
		core.project->CloseAudio();
	}
	if (!player) {
		core.project->CloseAudio();
		return;
	}
	AnnounceAudioPlayerOpened();
}

void AudioController::OnAudioProvider(agi::AudioProvider *new_provider)
{
	provider = new_provider;
	Stop();
	player.reset();
	OnAudioPlayerChanged();
}

void AudioController::SetTimingController(std::unique_ptr<AudioTimingController> new_controller)
{
	timing_controller = std::move(new_controller);
	if (timing_controller)
		timing_controller->AddUpdatedPrimaryRangeListener(&AudioController::OnTimingControllerUpdatedPrimaryRange, this);

	AnnounceTimingControllerChanged();
}

void AudioController::OnTimingControllerUpdatedPrimaryRange()
{
	if (playback_mode == PM_PrimaryRange)
		player->SetEndPosition(SamplesFromMilliseconds(timing_controller->GetPrimaryPlaybackRange().end()));
}

void AudioController::PlayRange(const TimeRange &range)
{
	if (!player) return;

	auto const start_sample = SamplesFromMilliseconds(range.begin());
	if (provider) {
		provider->SetPlaybackWindow(
			start_sample,
			SamplesFromMilliseconds(kPlaybackAheadMs),
			SamplesFromMilliseconds(kPlaybackBehindMs));
	}
	perf_trace::ResetAudioUiTimerInterval();
	player->Play(start_sample, SamplesFromMilliseconds(range.length()));
	playback_mode = PM_Range;
	// This is a UI refresh timer, not the device clock. On Windows the observed
	// wake-up cadence often lands closer to ~31 ms unless timer resolution is raised.
	playback_timer->Start(kAudioUiTimerRequestedMs);

	AnnouncePlaybackPosition(range.begin());
}

void AudioController::PlayPrimaryRange()
{
	PlayRange(GetPrimaryPlaybackRange());
	if (playback_mode == PM_Range)
		playback_mode = PM_PrimaryRange;
}

void AudioController::PlayToEndOfPrimary(int start_ms)
{
	PlayRange(TimeRange(start_ms, GetPrimaryPlaybackRange().end()));
	if (playback_mode == PM_Range)
		playback_mode = PM_PrimaryRange;
}

void AudioController::PlayToEnd(int start_ms)
{
	if (!player) return;

	int64_t start_sample = SamplesFromMilliseconds(start_ms);
	if (provider) {
		provider->SetPlaybackWindow(
			start_sample,
			SamplesFromMilliseconds(kPlaybackAheadMs),
			SamplesFromMilliseconds(kPlaybackBehindMs));
	}
	perf_trace::ResetAudioUiTimerInterval();
	player->Play(start_sample, provider->GetNumSamples()-start_sample);
	playback_mode = PM_ToEnd;
	// This is a UI refresh timer, not the device clock. On Windows the observed
	// wake-up cadence often lands closer to ~31 ms unless timer resolution is raised.
	playback_timer->Start(kAudioUiTimerRequestedMs);

	AnnouncePlaybackPosition(start_ms);
}

void AudioController::Stop()
{
	if (!player) return;

	player->Stop();
	playback_mode = PM_NotPlaying;
	playback_timer->Stop();
	perf_trace::ResetAudioUiTimerInterval();
	if (provider)
		provider->ClearPlaybackWindow();

	AnnouncePlaybackStop();
}

bool AudioController::IsPlaying()
{
	return player && playback_mode != PM_NotPlaying;
}

int AudioController::GetPlaybackPosition()
{
	if (!IsPlaying()) return 0;

	return MillisecondsFromSamples(player->GetCurrentPosition());
}

int AudioController::GetDuration() const
{
	if (!provider) return 0;
	return (provider->GetNumSamples() * 1000 + provider->GetSampleRate() - 1) / provider->GetSampleRate();
}

TimeRange AudioController::GetPrimaryPlaybackRange() const
{
	if (timing_controller)
		return timing_controller->GetPrimaryPlaybackRange();
	else
		return TimeRange{0, 0};
}

void AudioController::SetVolume(double volume)
{
	if (!player) return;
	player->SetVolume(volume);
}

int64_t AudioController::SamplesFromMilliseconds(int64_t ms) const
{
	if (!provider) return 0;
	return (ms * provider->GetSampleRate() + 999) / 1000;
}

int64_t AudioController::MillisecondsFromSamples(int64_t samples) const
{
	if (!provider) return 0;
	return samples * 1000 / provider->GetSampleRate();
}
