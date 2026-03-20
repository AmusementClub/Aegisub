#include "../../src/native_library.h"

#include <gtest/gtest.h>

#include <vector>

namespace {
using agi::native::BuildLibraryNameVariations;
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
