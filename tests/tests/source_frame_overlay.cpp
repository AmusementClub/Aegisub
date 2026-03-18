#include <main.h>

#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/source_frame.h"
#include "../../src/subtitle_overlay.h"
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
	EXPECT_EQ(99, source.planes[0].data[5]);
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

TEST(source_frame_overlay, source_frame_format_info_reports_semiplanar_and_planar_layouts) {
	auto nv12 = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
	EXPECT_EQ(SourceFrameColorFamily::YCbCr, nv12.color_family);
	EXPECT_EQ(2, nv12.plane_count);
	EXPECT_EQ(2, nv12.planes[1].components_per_sample);
	EXPECT_EQ(2, nv12.planes[1].bytes_per_sample);

	auto ycbcr420p10 = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
	EXPECT_EQ(SourceFrameColorFamily::YCbCr, ycbcr420p10.color_family);
	EXPECT_EQ(3, ycbcr420p10.plane_count);
	EXPECT_EQ(2, ycbcr420p10.planes[0].bytes_per_sample);
	EXPECT_EQ(10, ycbcr420p10.planes[2].bits_per_component);
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
