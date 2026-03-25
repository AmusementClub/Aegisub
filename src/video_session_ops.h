#pragma once

#include <libaegisub/fs_fwd.h>
#include <libaegisub/vfr.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class AsyncVideoProvider;

namespace aegisub::video_session_ops {

struct OpenedVideoSummary {
	agi::vfr::Framerate timecodes;
	std::vector<int> keyframes;
	std::string warning;
	std::optional<double> display_aspect_ratio_override;
	bool has_audio = false;
	bool has_subtitles = false;
};

using HasSubtitlesProbe = std::function<bool(agi::fs::path const&)>;

OpenedVideoSummary BuildOpenedVideoSummary(AsyncVideoProvider const& provider,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe = {});

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
