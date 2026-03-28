#pragma once

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>

namespace aegisub::ass_info_service {

struct AssInfoInspectRequest {
	agi::fs::path subtitle_path;
	std::string encoding;
};

struct AssInfoSnapshot {
	agi::fs::path subtitle_path;
	std::string format_name;
	std::string title;
	std::string script_type;
	std::string wrap_style;
	std::string scaled_border_and_shadow;
	int play_res_x = 0;
	int play_res_y = 0;
	int layout_res_x = 0;
	int layout_res_y = 0;
	size_t info_count = 0;
	size_t style_count = 0;
	size_t event_count = 0;
	size_t dialogue_count = 0;
	size_t comment_count = 0;
	size_t attachment_count = 0;
	size_t extradata_count = 0;
	std::string project_audio_file;
	std::string project_video_file;
	std::string project_timecodes_file;
	std::string project_keyframes_file;
};

struct AssInfoInspectResult {
	std::optional<AssInfoSnapshot> snapshot;
	std::string error;
};

AssInfoInspectResult Inspect(AssInfoInspectRequest const& request);

}
