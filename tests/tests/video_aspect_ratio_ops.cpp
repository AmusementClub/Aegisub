#include <main.h>

#include "../../src/video_aspect_ratio_ops.h"

TEST(video_aspect_ratio_ops, parses_decimal_and_fractional_formats) {
	auto decimal = aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("2.35");
	ASSERT_TRUE(decimal);
	EXPECT_DOUBLE_EQ(2.35, decimal.value);

	auto fractional = aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("16:9");
	ASSERT_TRUE(fractional);
	EXPECT_DOUBLE_EQ(16.0 / 9.0, fractional.value);

	auto division = aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("4/3");
	ASSERT_TRUE(division);
	EXPECT_DOUBLE_EQ(4.0 / 3.0, division.value);
}

TEST(video_aspect_ratio_ops, parses_resolution_formats_and_trims_whitespace) {
	auto lower_x = aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("853x480");
	ASSERT_TRUE(lower_x);
	EXPECT_DOUBLE_EQ(853.0 / 480.0, lower_x.value);

	auto upper_x = aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio(" 1920 X 1080 ");
	ASSERT_TRUE(upper_x);
	EXPECT_DOUBLE_EQ(1920.0 / 1080.0, upper_x.value);
}

TEST(video_aspect_ratio_ops, rejects_invalid_formats) {
	using aegisub::video_aspect_ratio_ops::ParseStatus;

	EXPECT_EQ(ParseStatus::InvalidFormat, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("").status);
	EXPECT_EQ(ParseStatus::InvalidFormat, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("abc").status);
	EXPECT_EQ(ParseStatus::InvalidFormat, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("16:9:4").status);
	EXPECT_EQ(ParseStatus::InvalidFormat, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("16:0").status);
}

TEST(video_aspect_ratio_ops, rejects_values_outside_supported_range) {
	using aegisub::video_aspect_ratio_ops::ParseStatus;

	EXPECT_EQ(ParseStatus::OutOfRange, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("0.4").status);
	EXPECT_EQ(ParseStatus::OutOfRange, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("5.1").status);
	EXPECT_EQ(ParseStatus::OutOfRange, aegisub::video_aspect_ratio_ops::ParseCustomAspectRatio("0x480").status);
}
