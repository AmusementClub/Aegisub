#pragma once

#include "playback_query_service.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::playback_session_service {

enum class PlaybackSessionStepKind {
	OpenMedia,
	ReopenMedia,
	CloseMedia,
	InstallPlayLine,
	PlayVideo,
	PlayLine,
	StopPlayback,
	Sleep,
	WaitPlaybackStop,
	JumpToTime,
	JumpToFrame,
	QueryMedia,
	QueryPlayback,
	AssertMedia,
	AssertPlaying,
	AssertAuthority
};

enum class PlaybackAuthorityKind {
	Video,
	Audio
};

struct PlaybackSessionStep {
	PlaybackSessionStepKind kind = PlaybackSessionStepKind::OpenMedia;
	int primary_value = 0;
	int secondary_value = 0;
	bool expected_first = false;
	bool expected_second = false;
	PlaybackAuthorityKind expected_authority = PlaybackAuthorityKind::Video;
	std::string source_text;
};

struct PlaybackSessionRequest {
	agi::fs::path video_path;
	agi::fs::path audio_path;
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	bool skip_audio = false;
	double audio_rate_scale = 1.0;
	int audio_quantum_ms = 0;
	std::optional<agi::fs::path> trace_dir;
	std::vector<PlaybackSessionStep> steps;
};

struct PlaybackSessionResult {
	int exit_code = 0;
	bool passed = false;
	size_t total_steps = 0;
	size_t completed_steps = 0;
	size_t open_count = 0;
	size_t reopen_count = 0;
	size_t close_count = 0;
	size_t query_count = 0;
	size_t play_count = 0;
	size_t playline_count = 0;
	size_t stop_count = 0;
	size_t jump_time_count = 0;
	size_t jump_frame_count = 0;
	agi::fs::path trace_dir;
	playback_query_service::ProjectMediaSnapshot final_media;
	playback_query_service::PlaybackStateSnapshot final_playback;
	std::string message;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	std::string actual_video_provider;
	std::string actual_video_decoder;
	bool video_provider_fallback = false;
	std::string video_provider_fallback_reason;
	std::string video_provider_attempts;
	std::string actual_audio_provider_factory;
	std::string actual_audio_provider;
	bool audio_provider_fallback = false;
	std::string audio_provider_fallback_reason;
	std::string audio_provider_attempts;
};

void RunAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done);

}
