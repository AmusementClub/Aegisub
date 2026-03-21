#include <libaegisub/access.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <gtest/gtest.h>

namespace {
bool TryCheckReadableMediaPath(agi::fs::path const& path, std::string& error) {
	error.clear();
	if (agi::IsNonFilesystemMediaPath(path))
		return true;

	try {
		agi::acs::CheckFileRead(path);
		return true;
	}
	catch (agi::fs::FileSystemError const& err) {
		error = err.GetMessage();
		return false;
	}
}
}

TEST(media_load_precheck, missing_real_file_skips_loader_and_reports_error) {
	bool loader_called = false;
	std::string error;

	if (TryCheckReadableMediaPath("data/nonexistent", error)) {
		loader_called = true;
	}
	EXPECT_FALSE(loader_called);
	EXPECT_NE(std::string::npos, error.find("File not found"));
}

TEST(media_load_precheck, existing_real_file_runs_loader) {
	bool loader_called = false;
	std::string error = "stale";

	if (TryCheckReadableMediaPath("data/file", error)) {
		loader_called = true;
	}
	EXPECT_TRUE(loader_called);
	EXPECT_TRUE(error.empty());
}

TEST(media_load_precheck, dummy_video_path_bypasses_filesystem_probe) {
	bool loader_called = false;
	std::string error = "stale";

	if (TryCheckReadableMediaPath("?dummy:23.976:100:1280:720:0:0:0:", error)) {
		loader_called = true;
	}
	EXPECT_TRUE(loader_called);
	EXPECT_TRUE(error.empty());
}

TEST(media_load_precheck, dummy_audio_path_bypasses_filesystem_probe) {
	bool loader_called = false;
	std::string error = "stale";

	if (TryCheckReadableMediaPath("dummy-audio:noise?sr=44100&bd=16&ch=1&ln=1000", error)) {
		loader_called = true;
	}
	EXPECT_TRUE(loader_called);
	EXPECT_TRUE(error.empty());
}
