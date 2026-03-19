#include <main.h>

#include "../../src/ffms_version.h"

#include <gtest/gtest.h>

TEST(ffms_version, packs_numeric_versions_in_ffms_layout) {
	EXPECT_EQ(((5 << 24) | (0 << 16) | (0 << 8) | 0), ffms::MakeVersion(5, 0, 0, 0));
	EXPECT_EQ(((2 << 24) | (31 << 16) | (0 << 8) | 0), ffms::MakeVersion(2, 31, 0, 0));
}

TEST(ffms_version, feature_support_requires_both_header_and_runtime_versions) {
	auto header = ffms::MakeVersion(5, 0, 0, 0);
	auto runtime = ffms::MakeVersion(2, 24, 0, 0);
	auto required = ffms::api_version::kVideoRotation;

	EXPECT_TRUE(ffms::VersionsSupportFeature(header, runtime, required));
	EXPECT_FALSE(ffms::VersionsSupportFeature(ffms::MakeVersion(2, 23, 0, 0), runtime, required));
	EXPECT_FALSE(ffms::VersionsSupportFeature(header, ffms::MakeVersion(2, 23, 0, 0), ffms::api_version::kVideoFlip));
}

TEST(ffms_version, header_version_predicate_matches_compiled_header) {
	EXPECT_EQ(FFMS_VERSION >= ffms::api_version::kFrameColorMetadata,
		ffms::HeaderVersionAtLeast(ffms::api_version::kFrameColorMetadata));
	EXPECT_EQ(FFMS_VERSION >= ffms::api_version::kVideoRotation,
		ffms::HeaderVersionAtLeast(ffms::api_version::kVideoRotation));
}
