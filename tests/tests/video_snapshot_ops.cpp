#include <main.h>

#include "../../src/video_snapshot_ops.h"

#include <libaegisub/fs.h>

#include <set>

TEST(video_snapshot_ops, uses_path_token_requires_non_empty_question_prefixed_option) {
	EXPECT_FALSE(aegisub::video_snapshot_ops::UsesPathToken(""));
	EXPECT_FALSE(aegisub::video_snapshot_ops::UsesPathToken("screenshots"));
	EXPECT_TRUE(aegisub::video_snapshot_ops::UsesPathToken("?video"));
}

TEST(video_snapshot_ops, resolve_path_token_falls_back_from_video_to_script_for_dummy_video) {
	EXPECT_EQ("?script", aegisub::video_snapshot_ops::ResolvePathToken("?video", true));
	EXPECT_EQ("?video/custom", aegisub::video_snapshot_ops::ResolvePathToken("?video/custom", false));
	EXPECT_EQ("?script", aegisub::video_snapshot_ops::ResolvePathToken("?script", true));
}

TEST(video_snapshot_ops, normalize_root_directory_falls_back_for_empty_and_root_paths) {
	auto home = agi::fs::PathFromString("C:/Users/test");

	EXPECT_EQ(home, aegisub::video_snapshot_ops::NormalizeRootDirectory({}, home));
	EXPECT_EQ(home, aegisub::video_snapshot_ops::NormalizeRootDirectory(agi::fs::PathFromString("/"), home));
	EXPECT_EQ(agi::fs::PathFromString("C:/shots"), aegisub::video_snapshot_ops::NormalizeRootDirectory(agi::fs::PathFromString("C:/shots"), home));
}

TEST(video_snapshot_ops, build_snapshot_base_path_uses_dummy_or_video_stem) {
	auto root = agi::fs::PathFromString("C:/shots");

	EXPECT_EQ(root / "clip", aegisub::video_snapshot_ops::BuildSnapshotBasePath(root, agi::fs::PathFromString("C:/video/clip.mkv"), false));
	EXPECT_EQ(root / "dummy", aegisub::video_snapshot_ops::BuildSnapshotBasePath(root, agi::fs::PathFromString("?dummy"), true));
}

TEST(video_snapshot_ops, build_next_snapshot_path_skips_existing_candidates) {
	auto base = agi::fs::PathFromString("C:/shots/clip");
	std::set<agi::fs::path> existing = {
		agi::fs::PathFromString("C:/shots/clip_001_42.png"),
		agi::fs::PathFromString("C:/shots/clip_002_42.png")
	};

	auto next = aegisub::video_snapshot_ops::BuildNextSnapshotPath(base, 42, [&](agi::fs::path const& candidate) {
		return existing.count(candidate) != 0;
	});

	EXPECT_EQ(agi::fs::PathFromString("C:/shots/clip_003_42.png"), next);
}
