#include <main.h>

#include "../../src/ffms_native_format_info.h"

#include <gtest/gtest.h>

TEST(ffms_native_format_info, maps_semiplanar_and_planar_families) {
	FFMSNativeFormatIds ids;
	ids.nv12 = 1;
	ids.p010le = 2;
	ids.yuv420p = 3;
	ids.yuv422p10le = 4;

	SourceFrameFormatInfo info;
	ASSERT_TRUE(TryGetFFMSNativeSourceFrameFormatInfo(1, ids, info));
	EXPECT_EQ(SourceFrameColorFamily::YCbCr, info.color_family);
	EXPECT_EQ(2, info.plane_count);
	EXPECT_EQ(2, info.planes[1].components_per_sample);
	EXPECT_EQ(2, info.planes[1].bytes_per_sample);

	ASSERT_TRUE(TryGetFFMSNativeSourceFrameFormatInfo(2, ids, info));
	EXPECT_EQ(10, info.planes[0].bits_per_component);
	EXPECT_EQ(4, info.planes[1].bytes_per_sample);
	EXPECT_EQ(6, info.planes[0].component_shift[0]);
	EXPECT_EQ(6, info.planes[1].component_shift[0]);
	EXPECT_EQ(22, info.planes[1].component_shift[1]);

	ASSERT_TRUE(TryGetFFMSNativeSourceFrameFormatInfo(3, ids, info));
	EXPECT_EQ(3, info.plane_count);
	EXPECT_EQ(2, info.planes[1].width_divisor);
	EXPECT_EQ(2, info.planes[1].height_divisor);

	ASSERT_TRUE(TryGetFFMSNativeSourceFrameFormatInfo(4, ids, info));
	EXPECT_EQ(2, info.planes[1].width_divisor);
	EXPECT_EQ(1, info.planes[1].height_divisor);
	EXPECT_EQ(10, info.planes[2].bits_per_component);
}

TEST(ffms_native_format_info, rejects_unknown_pixfmt_ids) {
	FFMSNativeFormatIds ids;
	ids.nv12 = 1;

	SourceFrameFormatInfo info;
	EXPECT_FALSE(TryGetFFMSNativeSourceFrameFormatInfo(99, ids, info));
	EXPECT_FALSE(TryGetFFMSNativeSourceFrameFormatInfo(-1, ids, info));
}
