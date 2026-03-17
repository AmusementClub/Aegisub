#include <main.h>

#include "../../src/modern_gl_overlay_upload_plan.h"
#include "../../src/subtitle_overlay.h"

namespace {
SubtitleOverlayStorage make_storage(int width, int height) {
	SubtitleOverlayStorage storage;
	storage.Reset(width, height, false);
	return storage;
}
}

TEST(modern_gl_overlay_upload_plan, invalid_overlay_hides_layer_but_preserves_resources) {
	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;

	auto plan = DecideModernGLOverlayUploadPlan(state, nullptr);
	EXPECT_EQ(ModernGLOverlayUploadAction::HideKeepResources, plan.action);
	EXPECT_TRUE(plan.next_state.has_allocated_resources);
	EXPECT_FALSE(plan.next_state.has_visible_content);
}

TEST(modern_gl_overlay_upload_plan, hidden_layer_can_reuse_existing_content_without_upload) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideModernGLOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(ModernGLOverlayUploadAction::ReuseExistingContent, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(modern_gl_overlay_upload_plan, hidden_layer_without_allocated_resources_requires_full_upload) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = false;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideModernGLOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(ModernGLOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_allocated_resources);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(modern_gl_overlay_upload_plan, hidden_layer_uses_dirty_upload_when_rects_are_available) {
	auto storage = make_storage(1920, 1080);
	storage.dirty_rects.push_back({ 100, 200, 300, 50 });
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideModernGLOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(ModernGLOverlayUploadAction::DirtyUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(modern_gl_overlay_upload_plan, layout_change_forces_full_upload) {
	auto storage = make_storage(1280, 720);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1280;
	overlay.canvas_height = 720;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideModernGLOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(ModernGLOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(modern_gl_overlay_upload_plan, composition_mode_change_forces_full_upload) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;

	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = false;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideModernGLOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(ModernGLOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}

TEST(modern_gl_overlay_upload_plan, force_full_upload_overrides_reuse_path) {
	auto storage = make_storage(1920, 1080);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = 1920;
	overlay.canvas_height = 1080;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	overlay.force_full_upload = true;

	ModernGLOverlayLayerState state;
	state.width = 1920;
	state.height = 1080;
	state.canvas_width = 1920;
	state.canvas_height = 1080;
	state.has_allocated_resources = true;
	state.has_visible_content = true;
	state.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	auto plan = DecideModernGLOverlayUploadPlan(state, &overlay);
	EXPECT_EQ(ModernGLOverlayUploadAction::FullUpload, plan.action);
	EXPECT_TRUE(plan.next_state.has_visible_content);
}
