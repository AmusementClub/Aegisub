#include <main.h>

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
