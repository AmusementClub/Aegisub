#pragma once

#include "playback_query_service.h"

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>

namespace agi {
	struct Context;
}

namespace aegisub::project_open_service {

struct PlaybackOpenOptions {
	agi::fs::path video_path;
	std::optional<agi::fs::path> audio_path;
	bool skip_audio = false;
};

struct ProjectOpenResult {
	bool opened = false;
	int error_code = 0;
	playback_query_service::ProjectMediaSnapshot media;
	std::string error;
};

ProjectOpenResult Open(agi::Context& context, PlaybackOpenOptions const& options);

}
