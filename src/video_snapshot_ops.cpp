#include "video_snapshot_ops.h"

#include "format.h"

#include <libaegisub/fs.h>
#include <libaegisub/string_utils.h>

namespace aegisub::video_snapshot_ops {

bool UsesPathToken(std::string const& option) {
	return !option.empty() && option[0] == '?';
}

std::string ResolvePathToken(std::string option, bool is_dummy_video) {
	if (is_dummy_video && agi::util::strings::starts_with(option, "?video"))
		return "?script";
	return option;
}

agi::fs::path NormalizeRootDirectory(agi::fs::path root_path, agi::fs::path const& home_directory) {
	if (root_path.empty()
		|| root_path == agi::fs::PathFromString("\\")
		|| root_path == agi::fs::PathFromString("/"))
		return home_directory;
	return root_path;
}

agi::fs::path BuildSnapshotBasePath(agi::fs::path const& root_path, agi::fs::path const& video_name, bool is_dummy_video) {
	return root_path / (is_dummy_video ? agi::fs::PathFromString("dummy") : video_name.stem());
}

agi::fs::path BuildNextSnapshotPath(agi::fs::path const& base_path,
                                    int frame_number,
                                    std::function<bool(agi::fs::path const&)> const& file_exists) {
	int session_shot_count = 1;
	agi::fs::path path;
	auto const base_dir = base_path.parent_path();
	auto const base_name = agi::fs::PathToString(base_path.filename());
	do {
		path = base_dir / agi::fs::PathFromString(agi::format("%s_%03d_%d.png", base_name, session_shot_count++, frame_number));
	} while (file_exists && file_exists(path));
	return path;
}

}
