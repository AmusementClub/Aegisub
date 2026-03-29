#pragma once

#include "playback_query_service.h"

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>
#include <vector>

namespace aegisub::media_inspect_service {

struct TrackChoiceInfo {
	int choice_index = 0;
	std::string display_name;
};

struct MediaInspectRequest {
	agi::fs::path video_path;
	agi::fs::path audio_path;
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	std::optional<int> video_track_index;
	std::optional<int> audio_track_index;
	std::optional<int> subtitle_track_index;
	bool skip_audio = false;
	double audio_rate_scale = 1.0;
	int audio_quantum_ms = 0;
	std::optional<agi::fs::path> trace_dir;
};

struct MediaInspectResult {
	int exit_code = 0;
	bool opened = false;
	playback_query_service::ProjectMediaSnapshot media;
	playback_query_service::PlaybackStateSnapshot playback;
	agi::fs::path trace_dir;
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
	std::vector<TrackChoiceInfo> video_track_choices;
	std::vector<TrackChoiceInfo> audio_track_choices;
	std::vector<TrackChoiceInfo> subtitle_track_choices;
};

MediaInspectResult Inspect(MediaInspectRequest const& request);

}
