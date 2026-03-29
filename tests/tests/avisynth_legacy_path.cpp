#include "../../src/avisynth_legacy_path.h"

#include <gtest/gtest.h>

#include <libaegisub/fs.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

TEST(avisynth_legacy_path, ascii_paths_are_legacy_safe) {
	auto const path = agi::fs::PathFromString("data/avisynth-legacy-safe");
	auto legacy_path = avisynth::TryGetLegacyPathString(path);

	ASSERT_TRUE(legacy_path.has_value());
	EXPECT_EQ("data/avisynth-legacy-safe", *legacy_path);
}

#ifdef _WIN32
TEST(avisynth_legacy_path, utf8_only_paths_without_short_name_are_rejected) {
	auto const utf8_path = std::string("data/") + "\xF0\x9F\x98\x80" + "-avisynth-legacy-unsafe";
	auto const path = agi::fs::PathFromString(utf8_path);

	auto legacy_path = avisynth::TryGetLegacyPathString(path);
	if (GetACP() == CP_UTF8) {
		ASSERT_TRUE(legacy_path.has_value());
		EXPECT_EQ(utf8_path, *legacy_path);
	}
	else {
		EXPECT_FALSE(legacy_path.has_value());
		EXPECT_NE(std::string::npos, avisynth::BuildLegacyPathFailureMessage("LoadPlugin", path).find("ANSI or 8.3-safe path"));
	}
}
#endif