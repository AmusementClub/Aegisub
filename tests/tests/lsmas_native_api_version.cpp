#include <main.h>

#include "../../src/lsmas_native_api.h"

#include <gtest/gtest.h>

TEST(lsmas_native_api_version, requires_same_major_at_least_1_0) {
	EXPECT_FALSE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(0, 255, 255)));
	EXPECT_TRUE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(1, 0, 0)));
	EXPECT_TRUE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(1, 0, 255)));
	EXPECT_TRUE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(1, 1, 0)));
	EXPECT_TRUE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(1, 1, 1)));
	EXPECT_TRUE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(1, 2, 0)));
	// Cross-major versions may change ABI layout; reject them.
	EXPECT_FALSE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(2, 0, 0)));
	EXPECT_FALSE(lsmas::IsSupportedApiVersion(LSMAS_NATIVE_MAKE_API_VERSION(2, 1, 0)));
}

TEST(lsmas_native_api_version, gates_yuv420p8_to_api_1_1) {
	auto const v1_0 = LSMAS_NATIVE_MAKE_API_VERSION(1, 0, 0);
	auto const v1_1 = LSMAS_NATIVE_MAKE_API_VERSION(1, 1, 0);
	auto const v2_0 = LSMAS_NATIVE_MAKE_API_VERSION(2, 0, 0);

	EXPECT_TRUE(lsmas::SupportsVideoFrameOutput(v1_0, LSMAS_VIDEO_FRAME_OUTPUT_BGRA));
	EXPECT_FALSE(lsmas::SupportsVideoFrameOutput(v1_0, LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8));
	EXPECT_TRUE(lsmas::SupportsVideoFrameOutput(v1_1, LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8));
	// int32_t parameter: unknown / out-of-range format codes are rejected without enum UB.
	EXPECT_FALSE(lsmas::SupportsVideoFrameOutput(v1_1, -1));
	EXPECT_FALSE(lsmas::SupportsVideoFrameOutput(v1_1, 99));
	// Unsupported major: no output formats are advertised.
	EXPECT_FALSE(lsmas::SupportsVideoFrameOutput(v2_0, LSMAS_VIDEO_FRAME_OUTPUT_BGRA));
	EXPECT_FALSE(lsmas::SupportsVideoFrameOutput(v2_0, LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8));
}
