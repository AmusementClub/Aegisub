#pragma once

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <string>

namespace aegisub::video_snapshot_ops {

bool UsesPathToken(std::string const& option);
std::string ResolvePathToken(std::string option, bool is_dummy_video);
agi::fs::path NormalizeRootDirectory(agi::fs::path root_path, agi::fs::path const& home_directory);
agi::fs::path BuildSnapshotBasePath(agi::fs::path const& root_path, agi::fs::path const& video_name, bool is_dummy_video);
agi::fs::path BuildNextSnapshotPath(agi::fs::path const& base_path,
                                    int frame_number,
                                    std::function<bool(agi::fs::path const&)> const& file_exists);

}
