#include <libaegisub/native_library.h>

#include <libaegisub/fs.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
using agi::native::BuildLibraryNameVariations;
using agi::native::BuildLibraryLoadProbes;
using agi::native::DefaultAppLocalLoadOptions;
using agi::native::EnumerateLibrariesInExecutableRelativeDirectory;

std::string Join(std::string_view left, std::string_view right) {
	return agi::fs::PathToString(agi::fs::PathFromString(std::string(left)) / agi::fs::PathFromString(std::string(right)));
}

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
			/ std::filesystem::path("aegisub-native-library-test-" + unique);
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

TEST(native_library, absolute_paths_are_loaded_as_is) {
#ifdef _WIN32
	auto const candidates = BuildLibraryNameVariations("C:\\runtime\\libplacebo.dll");
	ASSERT_EQ(1u, candidates.size());
	EXPECT_EQ("C:\\runtime\\libplacebo.dll", candidates[0]);
#else
	auto const candidates = BuildLibraryNameVariations("/usr/lib/libplacebo.so");
	ASSERT_EQ(1u, candidates.size());
	EXPECT_EQ("/usr/lib/libplacebo.so", candidates[0]);
#endif
}

TEST(native_library, unqualified_names_follow_dotnet_conventions) {
#ifdef _WIN32
	EXPECT_EQ((std::vector<std::string>{ "libplacebo", "libplacebo.dll" }),
		BuildLibraryNameVariations("libplacebo"));
	EXPECT_EQ((std::vector<std::string>{ "placebo.dll" }),
		BuildLibraryNameVariations("placebo.dll"));
#elif defined(__APPLE__)
	EXPECT_EQ((std::vector<std::string>{ "placebo.dylib", "libplacebo.dylib", "placebo", "libplacebo" }),
		BuildLibraryNameVariations("placebo"));
	EXPECT_EQ((std::vector<std::string>{ "libplacebo.dylib", "libplacebo" }),
		BuildLibraryNameVariations("libplacebo"));
#else
	EXPECT_EQ((std::vector<std::string>{ "placebo.so", "libplacebo.so", "placebo", "libplacebo" }),
		BuildLibraryNameVariations("placebo"));
	EXPECT_EQ((std::vector<std::string>{ "libplacebo.so", "libplacebo" }),
		BuildLibraryNameVariations("libplacebo"));
#endif
}

TEST(native_library, unix_suffixed_names_keep_dotnet_style_lib_prefix_variants) {
#ifdef _WIN32
	GTEST_SKIP();
#elif defined(__APPLE__)
	EXPECT_EQ((std::vector<std::string>{ "placebo.dylib", "libplacebo.dylib" }),
		BuildLibraryNameVariations("placebo.dylib"));
	EXPECT_EQ((std::vector<std::string>{ "placebo.1.dylib", "libplacebo.1.dylib" }),
		BuildLibraryNameVariations("placebo.1.dylib"));
#else
	EXPECT_EQ((std::vector<std::string>{ "placebo.so", "libplacebo.so" }),
		BuildLibraryNameVariations("placebo.so"));
	EXPECT_EQ((std::vector<std::string>{ "placebo.so.1", "libplacebo.so.1" }),
		BuildLibraryNameVariations("placebo.so.1"));
#endif
}

TEST(native_library, directory_components_only_suppress_unix_lib_prefix) {
#ifdef _WIN32
	EXPECT_EQ((std::vector<std::string>{ "runtimes\\win-x64\\native\\libplacebo", "runtimes\\win-x64\\native\\libplacebo.dll" }),
		BuildLibraryNameVariations("runtimes\\win-x64\\native\\libplacebo"));
#elif defined(__APPLE__)
	EXPECT_EQ((std::vector<std::string>{ "runtimes/osx-x64/native/placebo.dylib", "runtimes/osx-x64/native/placebo" }),
		BuildLibraryNameVariations("runtimes/osx-x64/native/placebo"));
#else
	EXPECT_EQ((std::vector<std::string>{ "runtimes/linux-x64/native/placebo.so", "runtimes/linux-x64/native/placebo" }),
		BuildLibraryNameVariations("runtimes/linux-x64/native/placebo"));
#endif
}

TEST(native_library, load_probes_search_relative_exe_directories_before_exe_directory) {
#ifdef _WIN32
	auto options = DefaultAppLocalLoadOptions(false);
	auto const probes = BuildLibraryLoadProbes("libplacebo", "C:\\app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("C:\\app", "runtimes\\libplacebo"),
		Join("C:\\app", "runtimes\\libplacebo.dll"),
		Join("C:\\app", "libplacebo"),
		Join("C:\\app", "libplacebo.dll"),
	}), NormalizePaths(probes));
#elif defined(__APPLE__)
	auto options = DefaultAppLocalLoadOptions(false);
	auto const probes = BuildLibraryLoadProbes("placebo", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "runtimes/placebo.dylib"),
		Join("/app", "runtimes/libplacebo.dylib"),
		Join("/app", "runtimes/placebo"),
		Join("/app", "runtimes/libplacebo"),
		Join("/app", "placebo.dylib"),
		Join("/app", "libplacebo.dylib"),
		Join("/app", "placebo"),
		Join("/app", "libplacebo"),
	}), NormalizePaths(probes));
#else
	auto options = DefaultAppLocalLoadOptions(false);
	auto const probes = BuildLibraryLoadProbes("placebo", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "runtimes/placebo.so"),
		Join("/app", "runtimes/libplacebo.so"),
		Join("/app", "runtimes/placebo"),
		Join("/app", "runtimes/libplacebo"),
		Join("/app", "placebo.so"),
		Join("/app", "libplacebo.so"),
		Join("/app", "placebo"),
		Join("/app", "libplacebo"),
	}), NormalizePaths(probes));
#endif
}

TEST(native_library, explicit_relative_path_is_resolved_relative_to_exe_without_search_dir_prefixes) {
#ifdef _WIN32
	auto options = DefaultAppLocalLoadOptions(true);
	auto const probes = BuildLibraryLoadProbes("path\\a.dll", "C:\\app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("C:\\app", "path\\a.dll"),
	}), NormalizePaths(probes));
#elif defined(__APPLE__)
	auto options = DefaultAppLocalLoadOptions(true);
	auto const probes = BuildLibraryLoadProbes("path/a", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "path/a.dylib"),
		Join("/app", "path/a"),
	}), NormalizePaths(probes));
#else
	auto options = DefaultAppLocalLoadOptions(true);
	auto const probes = BuildLibraryLoadProbes("path/a.so", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "path/a.so"),
	}), NormalizePaths(probes));
#endif
}

TEST(native_library, named_relative_search_directory_is_checked_before_exe_directory) {
#ifdef _WIN32
	agi::native::LibraryLoadOptions options;
	options.executable_relative_search_dirs.emplace_back("csri");
	auto const probes = BuildLibraryLoadProbes("vsfilter", "C:\\app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("C:\\app", "csri\\vsfilter"),
		Join("C:\\app", "csri\\vsfilter.dll"),
		Join("C:\\app", "vsfilter"),
		Join("C:\\app", "vsfilter.dll"),
	}), NormalizePaths(probes));
#elif defined(__APPLE__)
	agi::native::LibraryLoadOptions options;
	options.executable_relative_search_dirs.emplace_back("csri");
	auto const probes = BuildLibraryLoadProbes("vsfilter", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "csri/vsfilter.dylib"),
		Join("/app", "csri/libvsfilter.dylib"),
		Join("/app", "csri/vsfilter"),
		Join("/app", "csri/libvsfilter"),
		Join("/app", "vsfilter.dylib"),
		Join("/app", "libvsfilter.dylib"),
		Join("/app", "vsfilter"),
		Join("/app", "libvsfilter"),
	}), NormalizePaths(probes));
#else
	agi::native::LibraryLoadOptions options;
	options.executable_relative_search_dirs.emplace_back("csri");
	auto const probes = BuildLibraryLoadProbes("vsfilter", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "csri/vsfilter.so"),
		Join("/app", "csri/libvsfilter.so"),
		Join("/app", "csri/vsfilter"),
		Join("/app", "csri/libvsfilter"),
		Join("/app", "vsfilter.so"),
		Join("/app", "libvsfilter.so"),
		Join("/app", "vsfilter"),
		Join("/app", "libvsfilter"),
	}), NormalizePaths(probes));
#endif
}

TEST(native_library, parent_directory_segments_are_rejected_for_explicit_relative_paths) {
#ifdef _WIN32
	EXPECT_TRUE(BuildLibraryLoadProbes("..\\path\\a.dll", "C:\\app", DefaultAppLocalLoadOptions(false)).empty());
#else
	EXPECT_TRUE(BuildLibraryLoadProbes("../path/a.so", "/app", DefaultAppLocalLoadOptions(false)).empty());
#endif
}

TEST(native_library, parent_directory_segments_are_ignored_in_search_directories) {
#ifdef _WIN32
	auto options = DefaultAppLocalLoadOptions(false);
	options.executable_relative_search_dirs.clear();
	options.executable_relative_search_dirs.emplace_back("..\\runtimes");
	options.executable_relative_search_dirs.emplace_back("runtimes");
	auto const probes = BuildLibraryLoadProbes("libplacebo", "C:\\app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("C:\\app", "runtimes\\libplacebo"),
		Join("C:\\app", "runtimes\\libplacebo.dll"),
		Join("C:\\app", "libplacebo"),
		Join("C:\\app", "libplacebo.dll"),
	}), NormalizePaths(probes));
#elif defined(__APPLE__)
	auto options = DefaultAppLocalLoadOptions(false);
	options.executable_relative_search_dirs.clear();
	options.executable_relative_search_dirs.emplace_back("../runtimes");
	options.executable_relative_search_dirs.emplace_back("runtimes");
	auto const probes = BuildLibraryLoadProbes("placebo", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "runtimes/placebo.dylib"),
		Join("/app", "runtimes/libplacebo.dylib"),
		Join("/app", "runtimes/placebo"),
		Join("/app", "runtimes/libplacebo"),
		Join("/app", "placebo.dylib"),
		Join("/app", "libplacebo.dylib"),
		Join("/app", "placebo"),
		Join("/app", "libplacebo"),
	}), NormalizePaths(probes));
#else
	auto options = DefaultAppLocalLoadOptions(false);
	options.executable_relative_search_dirs.clear();
	options.executable_relative_search_dirs.emplace_back("../runtimes");
	options.executable_relative_search_dirs.emplace_back("runtimes");
	auto const probes = BuildLibraryLoadProbes("placebo", "/app", options);
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "runtimes/placebo.so"),
		Join("/app", "runtimes/libplacebo.so"),
		Join("/app", "runtimes/placebo"),
		Join("/app", "runtimes/libplacebo"),
		Join("/app", "placebo.so"),
		Join("/app", "libplacebo.so"),
		Join("/app", "placebo"),
		Join("/app", "libplacebo"),
	}), NormalizePaths(probes));
#endif
}

TEST(native_library, executable_relative_directory_enumeration_filters_dynamic_libraries_and_sorts_results) {
	TempDirectory temp;
	auto app_dir = temp.path / "app";
	auto plugin_dir = app_dir / "csri";
	TouchFile(plugin_dir / "z_last.txt");
	TouchFile(plugin_dir / "subdir" / "nested.dll");
#ifdef _WIN32
	TouchFile(plugin_dir / "a_first.dll");
	TouchFile(plugin_dir / "m_middle.dll");
#elif defined(__APPLE__)
	TouchFile(plugin_dir / "a_first.dylib");
	TouchFile(plugin_dir / "m_middle.dylib");
	TouchFile(plugin_dir / "ignore.so");
#else
	TouchFile(plugin_dir / "a_first.so");
	TouchFile(plugin_dir / "m_middle.so.1");
	TouchFile(plugin_dir / "ignore.dylib");
#endif

	auto libraries = EnumerateLibrariesInExecutableRelativeDirectory("csri", agi::fs::PathToString(app_dir));
#ifdef _WIN32
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		agi::fs::PathToString(plugin_dir / "a_first.dll"),
		agi::fs::PathToString(plugin_dir / "m_middle.dll"),
	}), NormalizePaths(libraries));
#elif defined(__APPLE__)
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		agi::fs::PathToString(plugin_dir / "a_first.dylib"),
		agi::fs::PathToString(plugin_dir / "m_middle.dylib"),
	}), NormalizePaths(libraries));
#else
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		agi::fs::PathToString(plugin_dir / "a_first.so"),
		agi::fs::PathToString(plugin_dir / "m_middle.so.1"),
	}), NormalizePaths(libraries));
#endif
}

TEST(native_library, executable_relative_directory_enumeration_rejects_parent_segments) {
	TempDirectory temp;
	auto app_dir = temp.path / "app";
	std::filesystem::create_directories(app_dir / "csri");
	EXPECT_TRUE(EnumerateLibrariesInExecutableRelativeDirectory("../csri", agi::fs::PathToString(app_dir)).empty());
}

TEST(native_library, system_fallback_is_only_added_when_requested) {
#ifdef _WIN32
	auto probes = BuildLibraryLoadProbes("ffms2", "C:\\app", DefaultAppLocalLoadOptions(true));
	ASSERT_GE(probes.size(), 2u);
	EXPECT_EQ("ffms2", probes[probes.size() - 2]);
	EXPECT_EQ("ffms2.dll", probes[probes.size() - 1]);
#elif defined(__APPLE__)
	auto probes = BuildLibraryLoadProbes("ffms2", "/app", DefaultAppLocalLoadOptions(true));
	ASSERT_GE(probes.size(), 4u);
	EXPECT_EQ("ffms2.dylib", probes[probes.size() - 4]);
	EXPECT_EQ("libffms2.dylib", probes[probes.size() - 3]);
	EXPECT_EQ("ffms2", probes[probes.size() - 2]);
	EXPECT_EQ("libffms2", probes[probes.size() - 1]);
#else
	auto probes = BuildLibraryLoadProbes("ffms2", "/app", DefaultAppLocalLoadOptions(true));
	ASSERT_GE(probes.size(), 4u);
	EXPECT_EQ("ffms2.so", probes[probes.size() - 4]);
	EXPECT_EQ("libffms2.so", probes[probes.size() - 3]);
	EXPECT_EQ("ffms2", probes[probes.size() - 2]);
	EXPECT_EQ("libffms2", probes[probes.size() - 1]);
#endif
}

TEST(native_library, system_fallback_is_absent_when_disabled) {
#ifdef _WIN32
	auto probes = BuildLibraryLoadProbes("ffms2", "C:\\app", DefaultAppLocalLoadOptions(false));
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("C:\\app", "runtimes\\ffms2"),
		Join("C:\\app", "runtimes\\ffms2.dll"),
		Join("C:\\app", "ffms2"),
		Join("C:\\app", "ffms2.dll"),
	}), NormalizePaths(probes));
#elif defined(__APPLE__)
	auto probes = BuildLibraryLoadProbes("ffms2", "/app", DefaultAppLocalLoadOptions(false));
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "runtimes/ffms2.dylib"),
		Join("/app", "runtimes/libffms2.dylib"),
		Join("/app", "runtimes/ffms2"),
		Join("/app", "runtimes/libffms2"),
		Join("/app", "ffms2.dylib"),
		Join("/app", "libffms2.dylib"),
		Join("/app", "ffms2"),
		Join("/app", "libffms2"),
	}), NormalizePaths(probes));
#else
	auto probes = BuildLibraryLoadProbes("ffms2", "/app", DefaultAppLocalLoadOptions(false));
	EXPECT_EQ(NormalizePaths(std::vector<std::string>{
		Join("/app", "runtimes/ffms2.so"),
		Join("/app", "runtimes/libffms2.so"),
		Join("/app", "runtimes/ffms2"),
		Join("/app", "runtimes/libffms2"),
		Join("/app", "ffms2.so"),
		Join("/app", "libffms2.so"),
		Join("/app", "ffms2"),
		Join("/app", "libffms2"),
	}), NormalizePaths(probes));
#endif
}
