#include <main.h>

#include "../../src/source_frame.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_renderer_opengl_overlay_upload_plan.h"
#include "../../src/subtitle_overlay.h"

namespace {
SubtitleOverlayStorage make_storage(int width, int height) {
	SubtitleOverlayStorage storage;
	storage.Reset(width, height, false);
	storage.has_visible_content = true;
	return storage;
}
}

TEST(video_renderer_opengl_overlay_upload_plan, invalid_overlay_hides_layer_but_preserves_resources) {
	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, nullptr);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::HideKeepResources, plan.action);
	EXPECT_TRUE(plan.next_state.has_allocated_resources);
	EXPECT_FALSE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, hidden_layer_requires_full_upload_before_becoming_visible_again) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, hidden_layer_without_allocated_resources_requires_full_upload) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = false;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_allocated_resources);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, hidden_layer_ignores_dirty_upload_and_rebuilds_surface) {
	auto storage = make_storage(1920, 1080);
	storage.dirty_rects.push_back({ 100, 200, 300, 50 });
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, layout_change_forces_full_upload) {
	auto storage = make_storage(1280, 720);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1280;
	overlay.canvas_height = 720;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, composition_mode_change_forces_full_upload) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, force_full_upload_overrides_reuse_path) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	overlay.force_full_upload = true;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, invisible_overlay_hides_layer_instead_of_reusing_old_texture) {
	auto storage = make_storage(1920, 1080);
	storage.has_visible_content = false;
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::HideKeepResources, plan.action);
	EXPECT_FALSE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, cropped_storage_overlay_with_dirty_rects_forces_full_upload) {
	auto storage = make_storage(4, 2);
	storage.dirty_rects.push_back({ 0, 0, 2, 2 });
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 8;
	overlay.canvas_height = 6;
	overlay.target_x = 1;
	overlay.target_y = 2;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(8, 6);
	geometry.visible_rect = { 2, 1, 4, 3 };

	auto adjusted = AdjustSubtitleOverlayForSourceGeometry(overlay, geometry);
	ASSERT_TRUE(adjusted.IsValid());
	EXPECT_TRUE(adjusted.force_full_upload);
	EXPECT_EQ(nullptr, adjusted.dirty_rects);
	EXPECT_EQ(0, adjusted.dirty_rect_count);

	OpenGLVideoRendererOverlayLayerState state;
	state.width = adjusted.width;
	state.height = adjusted.height;
	state.canvas_width = adjusted.canvas_width;
	state.canvas_height = adjusted.canvas_height;
	state.offset_x = adjusted.target_x;
	state.offset_y = adjusted.target_y;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &adjusted);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(video_renderer_opengl_overlay_upload_plan, cropped_storage_overlay_outside_visible_rect_hides_layer) {
	auto storage = make_storage(2, 1);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 8;
	overlay.canvas_height = 6;
	overlay.target_x = 0;
	overlay.target_y = 0;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(8, 6);
	geometry.visible_rect = { 3, 2, 4, 3 };

	auto adjusted = AdjustSubtitleOverlayForSourceGeometry(overlay, geometry);
	EXPECT_FALSE(adjusted.has_visible_content);

	OpenGLVideoRendererOverlayLayerState state;
	state.width = 2;
	state.height = 1;
	state.canvas_width = 8;
	state.canvas_height = 6;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, &adjusted);
	EXPECT_EQ(OpenGLVideoRendererOverlayUploadAction::HideKeepResources, plan.action);
	EXPECT_FALSE(plan.next_state.has_visible_content);
}
