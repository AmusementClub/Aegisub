#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <string>

namespace agi {
	struct ContextCoreSession;
	struct ConstContextCoreSession;
}

namespace aegisub::playback_query_service {

struct PlaybackStateSnapshot {
	bool has_video = false;
	bool has_audio = false;
	bool video_playing = false;
	bool audio_playing = false;
	bool playback_uses_audio_authority = false;
	int current_frame = 0;
	int current_video_time_ms = 0;
	int current_audio_time_ms = 0;
	int primary_playback_begin_ms = 0;
	int primary_playback_end_ms = 0;
};

struct ProjectMediaSnapshot {
	agi::fs::path video_path;
	agi::fs::path audio_path;
	bool has_video = false;
	bool has_audio = false;
	bool can_load_subtitles_from_video = false;
	int video_width = 0;
	int video_height = 0;
	int video_frame_count = 0;
	int video_duration_ms = 0;
	std::string video_decoder_name;
	int audio_sample_rate = 0;
	int64_t audio_num_samples = 0;
	int audio_duration_ms = 0;
	std::string audio_provider_name;
};

PlaybackStateSnapshot QueryPlaybackState(agi::ContextCoreSession const& core);
PlaybackStateSnapshot QueryPlaybackState(agi::ConstContextCoreSession const& core);
ProjectMediaSnapshot QueryProjectMedia(agi::ContextCoreSession const& core);
ProjectMediaSnapshot QueryProjectMedia(agi::ConstContextCoreSession const& core);

}
