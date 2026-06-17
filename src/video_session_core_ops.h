#pragma once

#include <libaegisub/fs_fwd.h>
#include <libaegisub/vfr.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agi { class NotificationSink; }

namespace aegisub::video_session_ops {

struct OpenedVideoMetadata {
	agi::vfr::Framerate timecodes;
	std::vector<int> keyframes;
	std::string warning;
	std::optional<double> display_aspect_ratio_override;
	bool has_audio = false;
};

struct OpenedVideoSummary {
	agi::vfr::Framerate timecodes;
	std::vector<int> keyframes;
	std::string warning;
	std::optional<double> display_aspect_ratio_override;
	bool has_audio = false;
	bool has_subtitles = false;
};

using HasSubtitlesProbe = std::function<bool(agi::fs::path const&)>;
using MruRemoveAction = std::function<void(char const*, agi::fs::path const&)>;

OpenedVideoSummary BuildOpenedVideoSummary(OpenedVideoMetadata const& metadata,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe = {});

bool HandleUnreadableVideoOpenPath(agi::fs::path const& path,
                                   std::string const& access_error,
                                   agi::NotificationSink& notification_sink,
                                   MruRemoveAction const& remove_mru = {});

struct PostOpenPlan {
	std::optional<double> display_aspect_ratio_override;
	bool auto_load_linked_audio = false;
	int initial_frame = 0;
};

PostOpenPlan PlanPostOpen(OpenedVideoSummary const& summary,
                          bool open_audio_enabled,
                          agi::fs::path const& current_audio_file,
                          agi::fs::path const& current_video_file);

}

