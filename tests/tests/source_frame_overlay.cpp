#include <main.h>

#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/source_frame.h"
#include "../../src/subtitle_overlay.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_frame.h"

namespace {
class OverlayCapableSubtitlesProvider final : public SubtitlesProvider {
public:
	bool overlay_called = false;
	bool legacy_called = false;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	bool RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double) override {
		overlay_called = true;
		EXPECT_TRUE(source.IsValid());
		EXPECT_TRUE(overlay.IsValid());
		EXPECT_EQ(SourceFramePixelFormat::Bgra8, source.pixel_format);
		EXPECT_EQ(SubtitleOverlayPixelFormat::Bgra8, overlay.pixel_format);
		overlay.planes[0].data[0] = 77;
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		legacy_called = true;
	}
};

class LegacyOnlySubtitlesProvider final : public SubtitlesProvider {
public:
	bool legacy_called = false;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	void DrawSubtitles(VideoFrame &dst, double) override {
		legacy_called = true;
		dst.data[0] = 42;
	}
};
}

TEST(source_frame_overlay, source_frame_view_reflects_video_frame) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = true;
	frame.data.resize(48);
	frame.data[5] = 99;

	auto source = MakeSourceFrameView(frame, "BT.709");

	ASSERT_TRUE(source.IsValid());
	EXPECT_EQ(4, source.width);
	EXPECT_EQ(3, source.height);
	EXPECT_TRUE(source.flipped);
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, source.output_mode);
	EXPECT_EQ(1, source.plane_count);
	EXPECT_EQ(frame.data.data(), source.planes[0].data);
	EXPECT_EQ(16, source.planes[0].stride);
	EXPECT_EQ("BT.709", source.color.matrix);
	EXPECT_EQ(SourceFrameChromaLocation::Unknown, source.chroma_location);
	EXPECT_FALSE(SourceFrameHasSubsampledChroma(source));
	EXPECT_EQ(99, source.planes[0].data[5]);
	EXPECT_EQ(4, source.geometry.storage_width);
	EXPECT_EQ(3, source.geometry.storage_height);
	EXPECT_EQ(0, source.geometry.visible_rect.x);
	EXPECT_EQ(0, source.geometry.visible_rect.y);
	EXPECT_EQ(4, source.geometry.visible_rect.width);
	EXPECT_EQ(3, source.geometry.visible_rect.height);
	EXPECT_EQ(0, source.geometry.rotation);
	EXPECT_FALSE(source.geometry.display_vflip);
	EXPECT_DOUBLE_EQ(1.0, source.geometry.pixel_aspect_ratio);
}

TEST(source_frame_overlay, visible_rect_helper_clamps_invalid_geometry_back_to_full_frame) {
	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(4, 3);
	geometry.visible_rect = { 4, 0, 2, 3 };

	auto visible = GetSourceFrameVisibleRect(geometry, 4, 3);
	EXPECT_EQ(0, visible.x);
	EXPECT_EQ(0, visible.y);
	EXPECT_EQ(4, visible.width);
	EXPECT_EQ(3, visible.height);
}

TEST(source_frame_overlay, render_canvas_layout_uses_visible_rect_as_output_canvas) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto source = MakeSourceFrameView(frame);
	source.geometry.visible_rect = { 1, 1, 2, 2 };

	auto layout = BuildVideoRenderCanvasLayout(source);
	EXPECT_EQ(2, layout.canvas_width);
	EXPECT_EQ(2, layout.canvas_height);
	EXPECT_EQ(-1, layout.offset_x);
	EXPECT_EQ(-1, layout.offset_y);
}

TEST(source_frame_overlay, source_storage_overlay_adjusts_to_visible_rect_canvas) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto overlay = MakeLegacyBgraSubtitleOverlayView(frame);
	overlay.canvas_width = 4;
	overlay.canvas_height = 3;
	overlay.target_x = 2;
	overlay.target_y = 1;

	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(4, 3);
	geometry.visible_rect = { 1, 1, 2, 2 };

	auto adjusted = AdjustSubtitleOverlayForSourceGeometry(overlay, geometry);
	EXPECT_EQ(2, adjusted.canvas_width);
	EXPECT_EQ(2, adjusted.canvas_height);
	EXPECT_EQ(1, adjusted.target_x);
	EXPECT_EQ(0, adjusted.target_y);
}

TEST(source_frame_overlay, source_visible_overlay_keeps_original_canvas_mapping) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto overlay = MakeLegacyBgraSubtitleOverlayView(frame);
	overlay.canvas_width = 4;
	overlay.canvas_height = 3;
	overlay.target_x = 2;
	overlay.target_y = 1;
	overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceVisible;

	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(4, 3);
	geometry.visible_rect = { 1, 1, 2, 2 };

	auto adjusted = AdjustSubtitleOverlayForSourceGeometry(overlay, geometry);
	EXPECT_EQ(4, adjusted.canvas_width);
	EXPECT_EQ(3, adjusted.canvas_height);
	EXPECT_EQ(2, adjusted.target_x);
	EXPECT_EQ(1, adjusted.target_y);
}

TEST(source_frame_overlay, legacy_color_space_parser_infers_renderer_facing_metadata) {
	auto color = SourceFrameColorMetadataFromLegacyColorSpace("TV.601");

	EXPECT_EQ("TV.601", color.matrix);
	EXPECT_EQ("BT.601", color.primaries);
	EXPECT_TRUE(color.transfer.empty());
	EXPECT_EQ(SourceFrameColorRange::Full, color.range);
}

TEST(source_frame_overlay, source_frame_view_preserves_full_color_metadata) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	SourceFrameColorMetadata color;
	color.matrix = "TV.709";
	color.primaries = "BT.2020";
	color.transfer = "PQ";
	color.range = SourceFrameColorRange::Full;

	auto source = MakeSourceFrameView(frame, color);

	EXPECT_EQ("TV.709", source.color.matrix);
	EXPECT_EQ("BT.2020", source.color.primaries);
	EXPECT_EQ("PQ", source.color.transfer);
	EXPECT_EQ(SourceFrameColorRange::Full, source.color.range);
}

TEST(source_frame_overlay, bgra_source_frame_can_carry_upstream_native_format_identity) {
	VideoFrame frame;
	frame.width = 4;
	frame.height = 3;
	frame.pitch = 16;
	frame.flipped = false;
	frame.data.resize(48);

	auto source = MakeSourceFrameView(frame);
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 24 };

	EXPECT_TRUE(source.IsValid());
	EXPECT_EQ(SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, source.native_format.format_namespace);
	EXPECT_EQ(24, source.native_format.format_id);
}

TEST(source_frame_overlay, bgra_fallback_view_preserves_reference_metadata) {
	VideoFrame source_storage;
	source_storage.width = 4;
	source_storage.height = 3;
	source_storage.pitch = 16;
	source_storage.flipped = false;
	source_storage.data.resize(48);

	auto reference = MakeSourceFrameView(source_storage, "TV.709");
	reference.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 42 };
	reference.chroma_location = SourceFrameChromaLocation::TopCenter;
	reference.geometry.storage_width = 6;
	reference.geometry.storage_height = 5;
	reference.geometry.visible_rect = { 1, 1, 3, 2 };
	reference.geometry.pixel_aspect_ratio = 1.5;

	VideoFrame composited = source_storage;
	auto fallback = MakeSourceFrameView(composited, reference);

	EXPECT_EQ(SourceFrameOutputMode::Bgra8, fallback.output_mode);
	EXPECT_EQ("TV.709", fallback.color.matrix);
	EXPECT_EQ(SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, fallback.native_format.format_namespace);
	EXPECT_EQ(42, fallback.native_format.format_id);
	EXPECT_EQ(SourceFrameChromaLocation::TopCenter, fallback.chroma_location);
	EXPECT_EQ(6, fallback.geometry.storage_width);
	EXPECT_EQ(5, fallback.geometry.storage_height);
	EXPECT_EQ(1, fallback.geometry.visible_rect.x);
	EXPECT_EQ(1, fallback.geometry.visible_rect.y);
	EXPECT_EQ(3, fallback.geometry.visible_rect.width);
	EXPECT_EQ(2, fallback.geometry.visible_rect.height);
	EXPECT_DOUBLE_EQ(1.5, fallback.geometry.pixel_aspect_ratio);
}

TEST(source_frame_overlay, source_frame_format_info_reports_semiplanar_and_planar_layouts) {
	auto nv12 = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
	EXPECT_EQ(SourceFrameColorFamily::YCbCr, nv12.color_family);
	EXPECT_EQ(2, nv12.plane_count);
	EXPECT_TRUE(SourceFrameHasSubsampledChroma(nv12));
	EXPECT_EQ(2, nv12.planes[1].components_per_sample);
	EXPECT_EQ(2, nv12.planes[1].bytes_per_sample);
	EXPECT_EQ(0, nv12.planes[1].component_shift[0]);
	EXPECT_EQ(8, nv12.planes[1].component_shift[1]);

	auto ycbcr420p10 = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
	EXPECT_EQ(SourceFrameColorFamily::YCbCr, ycbcr420p10.color_family);
	EXPECT_EQ(3, ycbcr420p10.plane_count);
	EXPECT_TRUE(SourceFrameHasSubsampledChroma(ycbcr420p10));
	EXPECT_EQ(2, ycbcr420p10.planes[0].bytes_per_sample);
	EXPECT_EQ(10, ycbcr420p10.planes[2].bits_per_component);
	EXPECT_EQ(0, ycbcr420p10.planes[0].component_shift[0]);

	auto p010 = MakeSemiplanar420SourceFrameFormatInfo(
		10,
		2,
		4,
		{ { 6, 0, 0, 0 } },
		{ { 6, 22, 0, 0 } });
	EXPECT_EQ(6, p010.planes[0].component_shift[0]);
	EXPECT_EQ(6, p010.planes[1].component_shift[0]);
	EXPECT_EQ(22, p010.planes[1].component_shift[1]);
}

TEST(source_frame_overlay, planar_ycbcr_frame_validation_uses_format_geometry) {
	unsigned char y[32] = { };
	unsigned char u[8] = { };
	unsigned char v[8] = { };

	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 123 };
	source.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
	source.width = 4;
	source.height = 4;
	source.plane_count = source.format_info.plane_count;
	source.planes[0] = { y, 8, 4, 4 };
	source.planes[1] = { u, 4, 2, 2 };
	source.planes[2] = { v, 4, 2, 2 };

	EXPECT_TRUE(source.IsValid());
	EXPECT_EQ(2, GetSourceFramePlaneWidth(source.format_info, source.width, 1));
	EXPECT_EQ(2, GetSourceFramePlaneHeight(source.format_info, source.height, 1));
}

TEST(source_frame_overlay, planar_ycbcr_frame_rejects_mismatched_plane_geometry) {
	unsigned char y[32] = { };
	unsigned char u[8] = { };
	unsigned char v[8] = { };

	SourceFrame source;
	source.output_mode = SourceFrameOutputMode::Native;
	source.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 123 };
	source.format_info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
	source.width = 4;
	source.height = 4;
	source.plane_count = source.format_info.plane_count;
	source.planes[0] = { y, 8, 4, 4 };
	source.planes[1] = { u, 4, 4, 2 };
	source.planes[2] = { v, 4, 2, 2 };

	EXPECT_FALSE(source.IsValid());
}

TEST(source_frame_overlay, legacy_overlay_view_reflects_video_frame) {
	VideoFrame frame;
	frame.width = 2;
	frame.height = 2;
	frame.pitch = 8;
	frame.flipped = false;
	frame.data.resize(16);

	auto overlay = MakeLegacyBgraSubtitleOverlayView(frame);
	ASSERT_TRUE(overlay.IsValid());
	EXPECT_EQ(frame.data.data(), overlay.planes[0].data);
	EXPECT_EQ(SubtitleOverlayCoordinateSpace::SourceStorage, overlay.coordinate_space);
	overlay.planes[0].data[3] = 11;
	EXPECT_EQ(11, frame.data[3]);
}

TEST(source_frame_overlay, provider_can_render_overlay_without_legacy_path) {
	VideoFrame frame;
	frame.width = 2;
	frame.height = 2;
	frame.pitch = 8;
	frame.flipped = false;
	frame.data.resize(16);

	auto source = MakeSourceFrameView(frame, "BT.709");
	auto overlay = MakeLegacyBgraSubtitleOverlayView(frame);
	OverlayCapableSubtitlesProvider provider;

	EXPECT_TRUE(provider.RenderOverlay(source, overlay, 1.0));
	EXPECT_TRUE(provider.overlay_called);
	EXPECT_FALSE(provider.legacy_called);
	EXPECT_EQ(77, frame.data[0]);
}

TEST(source_frame_overlay, legacy_provider_still_draws_into_video_frame) {
	VideoFrame frame;
	frame.width = 2;
	frame.height = 2;
	frame.pitch = 8;
	frame.flipped = false;
	frame.data.resize(16);

	LegacyOnlySubtitlesProvider provider;
	provider.DrawSubtitles(frame, 1.0);

	EXPECT_TRUE(provider.legacy_called);
	EXPECT_EQ(42, frame.data[0]);
}
