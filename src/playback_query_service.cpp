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

#include "playback_query_service.h"

#include "async_video_provider.h"
#include "audio_controller.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "time_range.h"
#include "video_controller.h"

#include <libaegisub/audio/provider.h>

namespace aegisub::playback_query_service {
namespace {

template<typename CoreSession>
PlaybackStateSnapshot QueryPlaybackStateImpl(CoreSession const& core) {
	PlaybackStateSnapshot snapshot;
	if (!core.project || !core.videoController || !core.audioController)
		return snapshot;

	snapshot.has_video = core.project->VideoProvider() != nullptr;
	snapshot.has_audio = core.project->AudioProvider() != nullptr;
	snapshot.video_playing = core.videoController->IsPlaying();
	snapshot.audio_playing = core.audioController->IsPlaying();
	snapshot.playback_uses_audio_authority = core.videoController->PlaybackUsesAudioAuthority();
	snapshot.current_frame = snapshot.has_video ? core.videoController->GetFrameN() : 0;
	snapshot.current_video_time_ms = snapshot.has_video ? core.videoController->TimeAtFrame(snapshot.current_frame) : 0;
	snapshot.current_audio_time_ms = snapshot.audio_playing ? core.audioController->GetPlaybackPosition() : 0;

	auto primary_range = core.audioController->GetPrimaryPlaybackRange();
	snapshot.primary_playback_begin_ms = primary_range.begin();
	snapshot.primary_playback_end_ms = primary_range.end();
	return snapshot;
}

template<typename CoreSession>
ProjectMediaSnapshot QueryProjectMediaImpl(CoreSession const& core) {
	ProjectMediaSnapshot snapshot;
	if (!core.project)
		return snapshot;

	snapshot.video_path = core.project->VideoName();
	snapshot.audio_path = core.project->AudioName();
	snapshot.has_video = core.project->VideoProvider() != nullptr;
	snapshot.has_audio = core.project->AudioProvider() != nullptr;
	snapshot.can_load_subtitles_from_video = core.project->CanLoadSubtitlesFromVideo();

	if (auto *video_provider = core.project->VideoProvider()) {
		snapshot.video_width = video_provider->GetWidth();
		snapshot.video_height = video_provider->GetHeight();
		snapshot.video_frame_count = video_provider->GetFrameCount();
		snapshot.video_duration_ms = core.videoController ? core.videoController->TimeAtFrame(snapshot.video_frame_count - 1, agi::vfr::END) : 0;
		snapshot.video_decoder_name = video_provider->GetDecoderName();
	}

	if (auto *audio_provider = core.project->AudioProvider()) {
		snapshot.audio_sample_rate = audio_provider->GetSampleRate();
		snapshot.audio_num_samples = audio_provider->GetNumSamples();
		snapshot.audio_duration_ms = snapshot.audio_sample_rate > 0
			? static_cast<int>((snapshot.audio_num_samples * 1000 + snapshot.audio_sample_rate - 1) / snapshot.audio_sample_rate)
			: 0;
		snapshot.audio_provider_name = audio_provider->GetMemoryStats().provider_name;
	}

	return snapshot;
}

}

PlaybackStateSnapshot QueryPlaybackState(agi::ContextCoreSession const& core) {
	return QueryPlaybackStateImpl(core);
}

PlaybackStateSnapshot QueryPlaybackState(agi::ConstContextCoreSession const& core) {
	return QueryPlaybackStateImpl(core);
}

ProjectMediaSnapshot QueryProjectMedia(agi::ContextCoreSession const& core) {
	return QueryProjectMediaImpl(core);
}

ProjectMediaSnapshot QueryProjectMedia(agi::ConstContextCoreSession const& core) {
	return QueryProjectMediaImpl(core);
}

}
