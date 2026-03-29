#pragma once

#include "playback_query_service.h"

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <string>

namespace agi {
	struct ContextCoreSession;
	struct ConstContextCoreSession;
}

namespace aegisub::project_query_service {

struct ProjectSessionSnapshot {
	playback_query_service::ProjectMediaSnapshot media;
	agi::fs::path subtitle_path;
	agi::fs::path timecodes_path;
	agi::fs::path keyframes_path;
	bool subtitle_file_loaded = false;
	bool subtitle_modified = false;
	bool timecodes_file_loaded = false;
	bool timecodes_loaded = false;
	bool keyframes_file_loaded = false;
	bool keyframes_loaded = false;
	std::size_t style_count = 0;
	std::size_t event_count = 0;
	std::size_t dialogue_count = 0;
	std::size_t comment_count = 0;
	std::size_t keyframe_count = 0;
	std::string title;
};

ProjectSessionSnapshot QueryProjectSession(agi::ContextCoreSession const& core);
ProjectSessionSnapshot QueryProjectSession(agi::ConstContextCoreSession const& core);

}
