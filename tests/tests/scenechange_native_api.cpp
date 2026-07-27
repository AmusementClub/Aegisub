#include <main.h>

#include "../../src/scenechange_native_api.h"

#include <gtest/gtest.h>

TEST(scenechange_native_api, maps_known_input_pixel_formats_to_capability_bits) {
	EXPECT_EQ(scenechange::kInputPixelFormatGray8Padded16,
		scenechange::GetInputPixelFormatMask(SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16));
	EXPECT_EQ(scenechange::kInputPixelFormatYuv420p8,
		scenechange::GetInputPixelFormatMask(SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8));
	EXPECT_EQ(0u, scenechange::GetInputPixelFormatMask(SCENECHANGE_PROVIDER_PIXEL_FORMAT_UNKNOWN));
	EXPECT_EQ(0u, scenechange::GetInputPixelFormatMask(99));
}

TEST(scenechange_native_api, reports_supported_input_pixel_formats) {
	EXPECT_TRUE(scenechange::SupportsInputPixelFormat(
		scenechange::kInputPixelFormatGray8Padded16,
		SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16));
	EXPECT_FALSE(scenechange::SupportsInputPixelFormat(
		scenechange::kInputPixelFormatGray8Padded16,
		SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8));
	EXPECT_TRUE(scenechange::SupportsInputPixelFormat(
		scenechange::kAllInputPixelFormats,
		SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8));
	EXPECT_FALSE(scenechange::SupportsInputPixelFormat(
		scenechange::kAllInputPixelFormats,
		SCENECHANGE_PROVIDER_PIXEL_FORMAT_UNKNOWN));
}

TEST(scenechange_native_api, maps_backends_to_cache_tokens_and_names) {
	EXPECT_STREQ("scxvid",
		scenechange::CacheTokenForBackend(scenechange::Api::Backend::ScxvidProvider));
	EXPECT_STREQ("wwxd",
		scenechange::CacheTokenForBackend(scenechange::Api::Backend::WwxdProvider));
	EXPECT_STREQ("scxvid",
		scenechange::BackendName(scenechange::Api::Backend::ScxvidProvider));
	EXPECT_STREQ("wwxd-provider",
		scenechange::BackendName(scenechange::Api::Backend::WwxdProvider));
	EXPECT_STREQ("none",
		scenechange::BackendName(scenechange::Api::Backend::None));
}
