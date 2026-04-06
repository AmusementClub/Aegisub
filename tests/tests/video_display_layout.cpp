#include <main.h>

#include "../../src/source_frame.h"
#include "../../src/video_display_layout.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_frame.h"

TEST(video_display_layout, fixed_size_uses_exact_video_rect_and_bottom_origin) {
	auto layout = BuildVideoDisplayViewportLayout(1920, 1080, 1280, 720, false);
	EXPECT_EQ(0, layout.viewport_left);
	EXPECT_EQ(1280, layout.viewport_width);
	EXPECT_EQ(360, layout.viewport_bottom);
	EXPECT_EQ(0, layout.viewport_top);
	EXPECT_EQ(720, layout.viewport_height);
}

TEST(video_display_layout, free_size_letterboxes_left_right_for_tall_target_aspect) {
	auto layout = BuildVideoDisplayViewportLayout(200, 200, 200, 200, true, 3.0 / 4.0);
	EXPECT_EQ(25, layout.viewport_left);
	EXPECT_EQ(150, layout.viewport_width);
	EXPECT_EQ(0, layout.viewport_bottom);
	EXPECT_EQ(0, layout.viewport_top);
	EXPECT_EQ(200, layout.viewport_height);
}

TEST(video_display_layout, free_size_letterboxes_top_bottom_for_wide_target_aspect) {
	auto layout = BuildVideoDisplayViewportLayout(200, 200, 200, 200, true, 16.0 / 9.0);
	EXPECT_EQ(0, layout.viewport_left);
	EXPECT_EQ(200, layout.viewport_width);
	EXPECT_EQ(43, layout.viewport_bottom);
	EXPECT_EQ(43, layout.viewport_top);
	EXPECT_EQ(113, layout.viewport_height);
}

TEST(video_display_layout, attached_content_layout_defaults_to_base_viewport_without_transform) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	auto content = BuildVideoDisplayContentLayout(base, 240, true);
	EXPECT_EQ(base.viewport_left, content.viewport_left);
	EXPECT_EQ(base.viewport_width, content.viewport_width);
	EXPECT_EQ(base.viewport_bottom, content.viewport_bottom);
	EXPECT_EQ(base.viewport_top, content.viewport_top);
	EXPECT_EQ(base.viewport_height, content.viewport_height);
}

TEST(video_display_layout, attached_content_layout_scales_around_base_viewport_center) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	auto content = BuildVideoDisplayContentLayout(base, 240, true, { 1.5, 0.0, 0.0 });
	EXPECT_EQ(-70, content.viewport_left);
	EXPECT_EQ(480, content.viewport_width);
	EXPECT_EQ(-25, content.viewport_top);
	EXPECT_EQ(270, content.viewport_height);
	EXPECT_EQ(-5, content.viewport_bottom);
}

TEST(video_display_layout, attached_content_layout_clamps_pan_using_viewport_height_units) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	auto content = BuildVideoDisplayContentLayout(base, 240, true, { 2.0, 5.0, -5.0 });
	EXPECT_EQ(242, content.viewport_left);
	EXPECT_EQ(640, content.viewport_width);
	EXPECT_EQ(-322, content.viewport_top);
	EXPECT_EQ(360, content.viewport_height);
	EXPECT_EQ(202, content.viewport_bottom);
}

TEST(video_display_layout, detached_content_layout_stays_equal_to_base_viewport_when_disabled) {
	VideoDisplayViewportLayout base = { 0, 200, 43, 43, 113 };
	auto content = BuildVideoDisplayContentLayout(base, 199, false, { 3.0, 1.0, -1.0 });
	EXPECT_EQ(base.viewport_left, content.viewport_left);
	EXPECT_EQ(base.viewport_width, content.viewport_width);
	EXPECT_EQ(base.viewport_bottom, content.viewport_bottom);
	EXPECT_EQ(base.viewport_top, content.viewport_top);
	EXPECT_EQ(base.viewport_height, content.viewport_height);
}

TEST(video_display_layout, source_storage_visible_display_and_viewport_spaces_form_explicit_chain) {
	VideoFrame frame;
	frame.width = 12;
	frame.height = 8;
	frame.pitch = 48;
	frame.flipped = false;
	frame.data.resize(384);

	auto source = MakeSourceFrameView(frame, "TV.709");
	source.geometry = MakeDefaultSourceFrameGeometry(12, 8);
	source.geometry.visible_rect = { 2, 1, 8, 6 };
	source.geometry.rotation = 90;
	source.geometry.display_vflip = true;

	auto visible = GetSourceFrameVisibleRect(source);
	EXPECT_EQ(2, visible.x);
	EXPECT_EQ(1, visible.y);
	EXPECT_EQ(8, visible.width);
	EXPECT_EQ(6, visible.height);

	auto canvas = BuildVideoRenderCanvasLayout(source);
	EXPECT_EQ(8, canvas.canvas_width);
	EXPECT_EQ(6, canvas.canvas_height);
	EXPECT_EQ(-2, canvas.offset_x);
	EXPECT_EQ(-1, canvas.offset_y);

	auto display = BuildVideoRenderOutputLayout(canvas, source.geometry);
	EXPECT_EQ(6, display.output_width);
	EXPECT_EQ(8, display.output_height);
	EXPECT_EQ(90, display.rotation);
	EXPECT_TRUE(display.display_vflip);

	auto viewport = BuildVideoDisplayViewportLayout(
		200,
		200,
		200,
		200,
		true,
		static_cast<double>(display.output_width) / display.output_height);
	EXPECT_EQ(25, viewport.viewport_left);
	EXPECT_EQ(150, viewport.viewport_width);
	EXPECT_EQ(0, viewport.viewport_top);
	EXPECT_EQ(0, viewport.viewport_bottom);
	EXPECT_EQ(200, viewport.viewport_height);
}

TEST(video_display_layout, display_aspect_ratio_helper_uses_baked_quarter_turn_par) {
	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(12, 8);
	geometry.visible_rect = { 2, 1, 8, 6 };
	geometry.rotation = 90;
	geometry.pixel_aspect_ratio = 1.25;

	auto display = GetSourceFrameDisplayOutputRect(geometry);
	EXPECT_EQ(6, display.width);
	EXPECT_EQ(8, display.height);
	EXPECT_DOUBLE_EQ(0.6, GetSourceFrameDisplayAspectRatio(geometry));
}
