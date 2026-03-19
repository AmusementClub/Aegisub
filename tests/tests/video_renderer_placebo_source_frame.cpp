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

TEST(video_renderer_placebo_source_frame, visible_rect_maps_to_placebo_crop_rect) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto source = MakeSourceFrameView(frame, "BT.709");
	source.geometry.visible_rect = { 1, 1, 2, 2 };

	auto crop = BuildPlaceboSourceFrameCropRect(source);
	EXPECT_FLOAT_EQ(1.0f, crop.x0);
	EXPECT_FLOAT_EQ(1.0f, crop.y0);
	EXPECT_FLOAT_EQ(3.0f, crop.x1);
	EXPECT_FLOAT_EQ(3.0f, crop.y1);
}

TEST(video_renderer_placebo_source_frame, invalid_visible_rect_falls_back_to_full_crop_rect) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto source = MakeSourceFrameView(frame, "BT.709");
	source.geometry.visible_rect = { 4, 0, 1, 3 };

	auto crop = BuildPlaceboSourceFrameCropRect(source);
	EXPECT_FLOAT_EQ(0.0f, crop.x0);
	EXPECT_FLOAT_EQ(0.0f, crop.y0);
	EXPECT_FLOAT_EQ(4.0f, crop.x1);
	EXPECT_FLOAT_EQ(3.0f, crop.y1);
}

TEST(video_renderer_placebo_source_frame, source_rotation_maps_to_placebo_rotation) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto source = MakeSourceFrameView(frame, "BT.709");

	source.geometry.rotation = 90;
	EXPECT_EQ(PL_ROTATION_270, BuildPlaceboSourceFrameRotation(source));

	source.geometry.rotation = 180;
	EXPECT_EQ(PL_ROTATION_180, BuildPlaceboSourceFrameRotation(source));

	source.geometry.rotation = 270;
	EXPECT_EQ(PL_ROTATION_90, BuildPlaceboSourceFrameRotation(source));

	source.geometry.rotation = -90;
	EXPECT_EQ(PL_ROTATION_90, BuildPlaceboSourceFrameRotation(source));
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

TEST(video_renderer_placebo_source_frame, native_planar_10bit_preserves_raw_component_depth_before_alignment) {
	unsigned short y[16] = { };
	unsigned short u[4] = { };
	unsigned short v[4] = { };

	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 4 };
	source.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
	source.width = 4;
	source.height = 4;
	source.plane_count = source.format_info.plane_count;
	source.planes[0] = { reinterpret_cast<unsigned char const*>(y), 8, 4, 4 };
	source.planes[1] = { reinterpret_cast<unsigned char const*>(u), 4, 2, 2 };
	source.planes[2] = { reinterpret_cast<unsigned char const*>(v), 4, 2, 2 };

	struct pl_plane_data plane = {};
	ASSERT_TRUE(BuildPlaceboNativePlaneData(source, 0, plane));
	EXPECT_EQ(10, plane.component_size[0]);
	EXPECT_EQ(0, plane.component_pad[0]);
}

TEST(video_renderer_placebo_source_frame, source_chroma_location_maps_to_placebo_enum) {
	EXPECT_EQ(PL_CHROMA_UNKNOWN, InferPlaceboChromaLocation(SourceFrameChromaLocation::Unknown));
	EXPECT_EQ(PL_CHROMA_LEFT, InferPlaceboChromaLocation(SourceFrameChromaLocation::Left));
	EXPECT_EQ(PL_CHROMA_CENTER, InferPlaceboChromaLocation(SourceFrameChromaLocation::Center));
	EXPECT_EQ(PL_CHROMA_TOP_LEFT, InferPlaceboChromaLocation(SourceFrameChromaLocation::TopLeft));
	EXPECT_EQ(PL_CHROMA_TOP_CENTER, InferPlaceboChromaLocation(SourceFrameChromaLocation::TopCenter));
	EXPECT_EQ(PL_CHROMA_BOTTOM_LEFT, InferPlaceboChromaLocation(SourceFrameChromaLocation::BottomLeft));
	EXPECT_EQ(PL_CHROMA_BOTTOM_CENTER, InferPlaceboChromaLocation(SourceFrameChromaLocation::BottomCenter));
}

TEST(video_renderer_placebo_source_frame, unknown_chroma_location_uses_mpv_compatible_fallback) {
	unsigned char y[16] = { };
	unsigned char u[4] = { };
	unsigned char v[4] = { };

	SourceFrame limited_420;
	limited_420.output_mode = SourceFrameOutputMode::Native;
	limited_420.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 3 };
	limited_420.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
	limited_420.width = 4;
	limited_420.height = 4;
	limited_420.plane_count = limited_420.format_info.plane_count;
	limited_420.color.range = SourceFrameColorRange::Limited;
	limited_420.planes[0] = { y, 4, 4, 4 };
	limited_420.planes[1] = { u, 2, 2, 2 };
	limited_420.planes[2] = { v, 2, 2, 2 };
	EXPECT_EQ(PL_CHROMA_LEFT, ResolvePlaceboChromaLocation(limited_420));

	auto full_420 = limited_420;
	full_420.color.range = SourceFrameColorRange::Full;
	EXPECT_EQ(PL_CHROMA_CENTER, ResolvePlaceboChromaLocation(full_420));

	auto planar444 = limited_420;
	planar444.format_info = MakePlanarYCbCrSourceFrameFormatInfo(1, 1, 8, 1);
	planar444.plane_count = planar444.format_info.plane_count;
	planar444.planes[1] = { u, 4, 4, 4 };
	planar444.planes[2] = { v, 4, 4, 4 };
	planar444.color.range = SourceFrameColorRange::Limited;
	EXPECT_EQ(PL_CHROMA_CENTER, ResolvePlaceboChromaLocation(planar444));

	full_420.chroma_location = SourceFrameChromaLocation::TopLeft;
	EXPECT_EQ(PL_CHROMA_TOP_LEFT, ResolvePlaceboChromaLocation(full_420));
}

TEST(video_renderer_placebo_source_frame, only_subsampled_native_ycbcr_requires_explicit_chroma_location) {
	unsigned char y[16] = { };
	unsigned char u[4] = { };
	unsigned char v[4] = { };
	unsigned char rgb[16] = { };

	SourceFrame planar420;
	planar420.output_mode = SourceFrameOutputMode::Native;
	planar420.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 3 };
	planar420.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
	planar420.width = 4;
	planar420.height = 4;
	planar420.plane_count = planar420.format_info.plane_count;
	planar420.chroma_location = SourceFrameChromaLocation::Center;
	planar420.planes[0] = { y, 4, 4, 4 };
	planar420.planes[1] = { u, 2, 2, 2 };
	planar420.planes[2] = { v, 2, 2, 2 };
	EXPECT_TRUE(PlaceboSourceFrameNeedsExplicitChromaLocation(planar420));

	SourceFrame planar444 = planar420;
	planar444.native_format.format_id = 5;
	planar444.format_info = MakePlanarYCbCrSourceFrameFormatInfo(1, 1, 8, 1);
	planar444.plane_count = planar444.format_info.plane_count;
	planar444.planes[1] = { u, 4, 4, 4 };
	planar444.planes[2] = { v, 4, 4, 4 };
	EXPECT_FALSE(PlaceboSourceFrameNeedsExplicitChromaLocation(planar444));

	SourceFrame native_rgb;
	native_rgb.output_mode = SourceFrameOutputMode::Native;
	native_rgb.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 99 };
	native_rgb.format_info = { SourceFrameColorFamily::Rgb, 1, { {
		{ 1, 1, 4, 4, 8, { { 16, 8, 0, 24 } } }, { }, { }, { }
	} } };
	native_rgb.width = 2;
	native_rgb.height = 2;
	native_rgb.plane_count = native_rgb.format_info.plane_count;
	native_rgb.chroma_location = SourceFrameChromaLocation::Left;
	native_rgb.planes[0] = { rgb, 8, 2, 2 };
	EXPECT_FALSE(PlaceboSourceFrameNeedsExplicitChromaLocation(native_rgb));

	VideoFrame bgra_storage;
	bgra_storage.width = 2;
	bgra_storage.height = 2;
	bgra_storage.pitch = 8;
	bgra_storage.flipped = false;
	bgra_storage.data.resize(16);
	auto bgra = MakeSourceFrameView(bgra_storage);
	EXPECT_FALSE(PlaceboSourceFrameNeedsExplicitChromaLocation(bgra));
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

TEST(video_renderer_placebo_source_frame, ffms_metadata_tokens_map_to_placebo_color_space) {
	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 3 };
	source.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
	source.width = 4;
	source.height = 4;
	source.plane_count = source.format_info.plane_count;
	unsigned char y[16] = { };
	unsigned char u[4] = { };
	unsigned char v[4] = { };
	source.planes[0] = { y, 4, 4, 4 };
	source.planes[1] = { u, 2, 2, 2 };
	source.planes[2] = { v, 2, 2, 2 };
	source.color.matrix = "TV.709";
	source.color.primaries = "Film C";
	source.color.transfer = "Gamma 2.2";
	source.color.range = SourceFrameColorRange::Limited;

	auto color = BuildPlaceboSourceFrameColorSpace(source);
	EXPECT_EQ(PL_COLOR_PRIM_FILM_C, color.primaries);
	EXPECT_EQ(PL_COLOR_TRC_GAMMA22, color.transfer);
}

#endif
