#include "video_session_ops.h"

#include "async_video_provider.h"

#include <libaegisub/fs.h>

namespace aegisub::video_session_ops {

OpenedVideoSummary BuildOpenedVideoSummary(AsyncVideoProvider const& provider,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe) {
	OpenedVideoSummary summary;
	summary.timecodes = provider.GetFPS();
	summary.keyframes = provider.GetKeyFrames();
	summary.warning = provider.GetWarning();
	summary.has_audio = provider.HasAudio();

	auto dar = provider.GetDAR();
	if (dar > 0.0)
		summary.display_aspect_ratio_override = dar;

	if (has_subtitles_probe && agi::fs::HasExtension(path, "mkv"))
		summary.has_subtitles = has_subtitles_probe(path);

	return summary;
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
