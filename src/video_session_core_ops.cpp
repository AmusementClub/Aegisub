#include "video_session_core_ops.h"

#include "ui_services.h"

#include <libaegisub/fs.h>

namespace aegisub::video_session_ops {

OpenedVideoSummary BuildOpenedVideoSummary(OpenedVideoMetadata const& metadata,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe) {
	OpenedVideoSummary summary;
	summary.timecodes = metadata.timecodes;
	summary.keyframes = metadata.keyframes;
	summary.warning = metadata.warning;
	summary.has_audio = metadata.has_audio;
	summary.display_aspect_ratio_override = metadata.display_aspect_ratio_override;

	if (has_subtitles_probe && agi::fs::HasExtension(path, "mkv"))
		summary.has_subtitles = has_subtitles_probe(path);

	return summary;
}

bool HandleUnreadableVideoOpenPath(agi::fs::path const& path,
                                   std::string const& access_error,
                                   agi::NotificationSink& notification_sink,
                                   MruRemoveAction const& remove_mru) {
	if (remove_mru)
		remove_mru("Video", path);
	notification_sink.ShowError("Error loading file", access_error);
	return false;
}

PostOpenPlan PlanPostOpen(OpenedVideoSummary const& summary,
                          bool open_audio_enabled,
                          agi::fs::path const& current_audio_file,
                          agi::fs::path const& current_video_file) {
	PostOpenPlan plan;
	plan.display_aspect_ratio_override = summary.display_aspect_ratio_override;
	plan.auto_load_linked_audio =
		open_audio_enabled &&
		summary.has_audio &&
		current_audio_file != current_video_file;
	return plan;
}

}

