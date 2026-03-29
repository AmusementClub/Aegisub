#include <main.h>

#include "../../src/video_render_routing.h"

namespace {
VideoRenderPacket make_packet(bool has_overlay, SubtitleOverlayCompositionMode composition_mode) {
	VideoRenderPacket packet;
	packet.has_subtitle_overlay = has_overlay;
	packet.subtitle_overlay.pixel_format = SubtitleOverlayPixelFormat::Bgra8;
	packet.subtitle_overlay.width = 16;
	packet.subtitle_overlay.height = 16;
	packet.subtitle_overlay.canvas_width = 16;
	packet.subtitle_overlay.canvas_height = 16;
	packet.subtitle_overlay.plane_count = 1;
	packet.subtitle_overlay.planes[0].data = reinterpret_cast<unsigned char*>(0x1);
	packet.subtitle_overlay.composition_mode = composition_mode;
	packet.subtitle_overlay.premultiplied_alpha =
		composition_mode == SubtitleOverlayCompositionMode::PremultipliedAlpha;
	return packet;
}

std::shared_ptr<VideoFrame> make_frame() {
	auto frame = std::make_shared<VideoFrame>();
	frame->width = 2;
	frame->height = 2;
	frame->pitch = 8;
	frame->data.resize(16);
	return frame;
}
}

TEST(video_render_routing, source_only_packet_uses_source_frame_path) {
	auto packet = make_packet(false, SubtitleOverlayCompositionMode::Unsupported);

	EXPECT_EQ(
		VideoRenderRoutingMode::SourceFrameOnly,
		DecideVideoRenderRouting(packet, false));
	EXPECT_EQ(
		VideoRenderRoutingMode::SourceFrameOnly,
		DecideVideoRenderRouting(packet, true));
}

TEST(video_render_routing, direct_overlay_prefers_primary_renderer_when_supported) {
	auto packet = make_packet(true, SubtitleOverlayCompositionMode::PremultipliedAlpha);

	EXPECT_EQ(
		VideoRenderRoutingMode::PrimaryRendererDirectOverlay,
		DecideVideoRenderRouting(packet, true));
}

TEST(video_render_routing, direct_overlay_uses_secondary_renderer_when_primary_cannot_consume_it) {
	auto packet = make_packet(true, SubtitleOverlayCompositionMode::OpaqueReplace);

	EXPECT_EQ(
		VideoRenderRoutingMode::SecondaryRendererDirectOverlay,
		DecideVideoRenderRouting(packet, false));
}

TEST(video_render_routing, unsupported_overlay_falls_back_to_composited_frame) {
	auto packet = make_packet(true, SubtitleOverlayCompositionMode::Unsupported);

	EXPECT_EQ(
		VideoRenderRoutingMode::FallbackCompositedFrame,
		DecideVideoRenderRouting(packet, false));
	EXPECT_EQ(
		VideoRenderRoutingMode::FallbackCompositedFrame,
		DecideVideoRenderRouting(packet, true));
}

TEST(video_render_routing, invalid_direct_overlay_still_falls_back_to_composited_frame) {
	VideoRenderPacket packet;
	packet.has_subtitle_overlay = true;
	packet.subtitle_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	packet.subtitle_overlay.premultiplied_alpha = true;

	EXPECT_EQ(
		VideoRenderRoutingMode::FallbackCompositedFrame,
		DecideVideoRenderRouting(packet, false));
	EXPECT_EQ(
		VideoRenderRoutingMode::FallbackCompositedFrame,
		DecideVideoRenderRouting(packet, true));
}

TEST(video_render_routing, compatibility_packet_without_overlay_uses_composited_frame_fallback) {
	VideoRenderPacket packet;
	packet.source_frame_storage = make_frame();
	packet.composited_frame_storage = make_frame();

	EXPECT_TRUE(packet.HasDistinctCompositedFrame());
	EXPECT_EQ(
		VideoRenderRoutingMode::FallbackCompositedFrame,
		DecideVideoRenderRouting(packet, false));
	EXPECT_EQ(
		VideoRenderRoutingMode::FallbackCompositedFrame,
		DecideVideoRenderRouting(packet, true));
}

TEST(video_render_routing, shared_frame_storage_without_overlay_stays_on_source_frame_path) {
	VideoRenderPacket packet;
	packet.source_frame_storage = make_frame();
	packet.composited_frame_storage = packet.source_frame_storage;

	EXPECT_FALSE(packet.HasDistinctCompositedFrame());
	EXPECT_EQ(
		VideoRenderRoutingMode::SourceFrameOnly,
		DecideVideoRenderRouting(packet, false));
	EXPECT_EQ(
		VideoRenderRoutingMode::SourceFrameOnly,
		DecideVideoRenderRouting(packet, true));
}
