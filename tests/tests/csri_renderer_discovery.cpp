#include <main.h>

#include "../../src/csri_renderer_discovery.h"

#include <libaegisub/fs.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
std::vector<std::string> NormalizePaths(std::vector<std::string> paths) {
	for (auto& path : paths)
		path = agi::fs::PathToGenericString(agi::fs::PathFromString(path).lexically_normal());
	return paths;
}

struct TempDirectory {
	std::filesystem::path path;

	TempDirectory() {
		auto unique = std::to_string(
			static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
		path = std::filesystem::temp_directory_path()
			/ std::filesystem::path("aegisub-csri-discovery-test-" + unique);
		std::filesystem::create_directories(path);
	}

	~TempDirectory() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};

void TouchFile(std::filesystem::path const& path) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << "x";
}
}

TEST(csri_renderer_discovery, enumerates_only_app_relative_csri_directory) {
	TempDirectory temp;
	auto app_dir = temp.path / "app";
	auto csri_dir = app_dir / "csri";
#ifdef _WIN32
	TouchFile(csri_dir / "vsfilter.dll");
	TouchFile(csri_dir / "other.txt");
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		agi::fs::PathToString(csri_dir / "vsfilter.dll"),
	}), NormalizePaths(csri::EnumerateRendererLibraryFiles(agi::fs::PathToString(app_dir))));
#elif defined(__APPLE__)
	TouchFile(csri_dir / "vsfilter.dylib");
	TouchFile(csri_dir / "other.txt");
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		agi::fs::PathToString(csri_dir / "vsfilter.dylib"),
	}), NormalizePaths(csri::EnumerateRendererLibraryFiles(agi::fs::PathToString(app_dir))));
#else
	TouchFile(csri_dir / "vsfilter.so");
	TouchFile(csri_dir / "other.txt");
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		agi::fs::PathToString(csri_dir / "vsfilter.so"),
	}), NormalizePaths(csri::EnumerateRendererLibraryFiles(agi::fs::PathToString(app_dir))));
#endif
}
