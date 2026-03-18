#include <main.h>

#ifdef WITH_LIBPLACEBO

#include "../../src/video_renderer_placebo_source_frame.h"

#include <gtest/gtest.h>

TEST(video_renderer_placebo_source_frame, bgra_repr_stays_rgb_full_range) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 2;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(32);

	auto source = MakeSourceFrameView(frame, "BT.709");
	auto repr = BuildPlaceboSourceFrameRepr(source);

	EXPECT_EQ(PL_COLOR_SYSTEM_RGB, repr.sys);
	EXPECT_EQ(PL_COLOR_LEVELS_FULL, repr.levels);
	EXPECT_EQ(8, repr.bits.sample_depth);
	EXPECT_EQ(8, repr.bits.color_depth);
	EXPECT_EQ(0, repr.bits.bit_shift);
}

TEST(video_renderer_placebo_source_frame, native_p010_repr_and_plane_data_preserve_bit_shift) {
	unsigned char y[8] = { };
	unsigned char uv[8] = { };

	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 2 };
	source.format_info = MakeSemiplanar420SourceFrameFormatInfo(
		10,
		2,
		4,
		{ { 6, 0, 0, 0 } },
		{ { 6, 22, 0, 0 } });
	source.width = 2;
	source.height = 2;
	source.plane_count = source.format_info.plane_count;
	source.planes[0] = { y, 4, 2, 2 };
	source.planes[1] = { uv, 4, 1, 1 };
	source.color.matrix = "TV.709";
	source.color.primaries = "BT.709";
	source.color.range = SourceFrameColorRange::Limited;

	auto repr = BuildPlaceboSourceFrameRepr(source);
	EXPECT_EQ(PL_COLOR_SYSTEM_BT_709, repr.sys);
	EXPECT_EQ(PL_COLOR_LEVELS_LIMITED, repr.levels);
	EXPECT_EQ(16, repr.bits.sample_depth);
	EXPECT_EQ(10, repr.bits.color_depth);
	EXPECT_EQ(6, repr.bits.bit_shift);

	struct pl_plane_data plane = {};
	ASSERT_TRUE(BuildPlaceboNativePlaneData(source, 1, plane));
	EXPECT_EQ(1, plane.width);
	EXPECT_EQ(1, plane.height);
	EXPECT_EQ(4u, plane.pixel_stride);
	EXPECT_EQ(4u, plane.row_stride);
	EXPECT_EQ(10, plane.component_size[0]);
	EXPECT_EQ(10, plane.component_size[1]);
	EXPECT_EQ(6, plane.component_pad[0]);
	EXPECT_EQ(22, plane.component_pad[1]);
	EXPECT_EQ(PL_CHANNEL_CB, plane.component_map[0]);
	EXPECT_EQ(PL_CHANNEL_CR, plane.component_map[1]);
}

TEST(video_renderer_placebo_source_frame, native_planar_420_uses_ycbcr_channel_mapping) {
	unsigned char y[16] = { };
	unsigned char u[4] = { };
	unsigned char v[4] = { };

	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 3 };
	source.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
	source.width = 4;
	source.height = 4;
	source.plane_count = source.format_info.plane_count;
	source.planes[0] = { y, 4, 4, 4 };
	source.planes[1] = { u, 2, 2, 2 };
	source.planes[2] = { v, 2, 2, 2 };

	struct pl_plane_data y_plane = {};
	struct pl_plane_data u_plane = {};
	struct pl_plane_data v_plane = {};
	ASSERT_TRUE(BuildPlaceboNativePlaneData(source, 0, y_plane));
	ASSERT_TRUE(BuildPlaceboNativePlaneData(source, 1, u_plane));
	ASSERT_TRUE(BuildPlaceboNativePlaneData(source, 2, v_plane));
	EXPECT_EQ(PL_CHANNEL_Y, y_plane.component_map[0]);
	EXPECT_EQ(PL_CHANNEL_CB, u_plane.component_map[0]);
	EXPECT_EQ(PL_CHANNEL_CR, v_plane.component_map[0]);
}

TEST(video_renderer_placebo_source_frame, native_rgb_plane_uses_rgb_channel_mapping) {
	unsigned char rgba[16] = { };

	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 99 };
	source.format_info = { SourceFrameColorFamily::Rgb, 1, { {
		{ 1, 1, 4, 4, 8, { { 16, 8, 0, 24 } } }, { }, { }, { }
	} } };
	source.width = 2;
	source.height = 2;
	source.plane_count = source.format_info.plane_count;
	source.planes[0] = { rgba, 8, 2, 2 };

	struct pl_plane_data plane = {};
	ASSERT_TRUE(BuildPlaceboNativePlaneData(source, 0, plane));
	EXPECT_EQ(PL_CHANNEL_R, plane.component_map[0]);
	EXPECT_EQ(PL_CHANNEL_G, plane.component_map[1]);
	EXPECT_EQ(PL_CHANNEL_B, plane.component_map[2]);
	EXPECT_EQ(PL_CHANNEL_A, plane.component_map[3]);
}

#endif
