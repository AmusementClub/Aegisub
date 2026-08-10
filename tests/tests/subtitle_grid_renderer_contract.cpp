#include "../../src/grid_core/subtitle_grid_renderer_contract.h"
#include "../../src/grid_core/windows_text_raster_policy.h"
#include "../../src/skia_runtime/skia_runtime_feature.h"

#include <gtest/gtest.h>

namespace grid = aegisub::grid;

TEST(subtitle_grid_renderer_contract, runtime_environment_override_precedes_setting) {
	using aegisub::skia::ResolveRuntimeFeatureEnabled;
	EXPECT_FALSE(ResolveRuntimeFeatureEnabled(nullptr, false));
	EXPECT_TRUE(ResolveRuntimeFeatureEnabled(nullptr, true));
	EXPECT_FALSE(ResolveRuntimeFeatureEnabled("0", true));
	EXPECT_FALSE(ResolveRuntimeFeatureEnabled("false", true));
	EXPECT_TRUE(ResolveRuntimeFeatureEnabled("1", false));
	EXPECT_TRUE(ResolveRuntimeFeatureEnabled("yes", false));
}

TEST(subtitle_grid_renderer_contract, wx_request_never_transitions_or_queues_fallback) {
	grid::SubtitleGridRendererState state(false);
	EXPECT_EQ(grid::SubtitleGridRendererBackend::Wx, state.Requested());
	EXPECT_EQ(grid::SubtitleGridRendererBackend::Wx, state.Active());
	state.MarkPresented();
	EXPECT_FALSE(state.Presented());
	EXPECT_EQ(grid::SubtitleGridRendererFailureAction::None, state.MarkFailed("ignored"));
	EXPECT_FALSE(state.Failed());
}

TEST(subtitle_grid_renderer_contract, skia_failure_queues_one_whole_frame_wx_refresh) {
	grid::SubtitleGridRendererState state(true);
	EXPECT_EQ(grid::SubtitleGridRendererBackend::Skia, state.Active());
	state.MarkPresented();
	EXPECT_TRUE(state.Presented());
	EXPECT_EQ(
		grid::SubtitleGridRendererFailureAction::QueueWxFullRefresh,
		state.MarkFailed("raster failed"));
	EXPECT_TRUE(state.Failed());
	EXPECT_EQ(grid::SubtitleGridRendererBackend::Wx, state.Active());
	EXPECT_EQ(
		grid::SubtitleGridRendererFailureAction::None,
		state.MarkFailed("duplicate failure"));
}

TEST(subtitle_grid_renderer_contract, paragraph_direction_uses_first_strong_character) {
	using Direction = grid::SubtitleGridParagraphDirection;
	EXPECT_EQ(Direction::LeftToRight,
		grid::ResolveSubtitleGridParagraphDirection("123 --"));
	EXPECT_EQ(Direction::LeftToRight,
		grid::ResolveSubtitleGridParagraphDirection("123 English \xD7\x90\xD7\x91\xD7\x92"));
	EXPECT_EQ(Direction::RightToLeft,
		grid::ResolveSubtitleGridParagraphDirection("123 \xD7\x90\xD7\x91\xD7\x92 English"));
	EXPECT_EQ(Direction::RightToLeft,
		grid::ResolveSubtitleGridParagraphDirection("\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9"));
}

TEST(subtitle_grid_renderer_contract, backing_scale_requires_a_one_to_one_bridge) {
	EXPECT_TRUE(grid::SupportsSkiaGridContentScale(1.0, false));
	EXPECT_FALSE(grid::SupportsSkiaGridContentScale(1.25, false));
	EXPECT_FALSE(grid::SupportsSkiaGridContentScale(2.0, false));
	EXPECT_TRUE(grid::SupportsSkiaGridContentScale(1.25, true));
	EXPECT_FALSE(grid::SupportsSkiaGridContentScale(0.0, true));
}

TEST(windows_text_raster_policy, remote_session_signals_are_conservative) {
	EXPECT_EQ(grid::RemoteSessionState::Remote,
		grid::ClassifyRemoteSession({true, 1u, 1u}));
	EXPECT_EQ(grid::RemoteSessionState::Local,
		grid::ClassifyRemoteSession({false, 7u, 7u}));
	EXPECT_EQ(grid::RemoteSessionState::Remote,
		grid::ClassifyRemoteSession({false, 7u, 8u}));
	EXPECT_EQ(grid::RemoteSessionState::Unknown,
		grid::ClassifyRemoteSession({false, 7u, std::nullopt}));
	EXPECT_EQ(grid::RemoteSessionState::Unknown,
		grid::ClassifyRemoteSession({false, std::nullopt, 7u}));
}

TEST(windows_text_raster_policy, grayscale_is_default_and_clear_type_requires_explicit_safe_opt_in) {
	grid::WindowsTextRasterConditions conditions {
		grid::RemoteSessionState::Local,
		grid::TextPixelGeometry::Rgb,
		true,
		true,
		true,
	};
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));

	conditions.local_clear_type_opt_in = true;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::ClearType,
		grid::SelectWindowsTextAntialiasPolicy(conditions));

	conditions.remote_state = grid::RemoteSessionState::Remote;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
	conditions.remote_state = grid::RemoteSessionState::Unknown;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
	conditions.remote_state = grid::RemoteSessionState::Local;

	conditions.pixel_geometry = grid::TextPixelGeometry::FlatOrUnknown;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
	conditions.pixel_geometry = grid::TextPixelGeometry::Bgr;
	conditions.opaque_target = false;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
	conditions.opaque_target = true;
	conditions.one_to_one_present = false;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
	conditions.one_to_one_present = true;
	conditions.clear_type_enabled = false;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
	conditions.clear_type_enabled = true;
	conditions.local_clear_type_opt_in = false;
	EXPECT_EQ(grid::WindowsTextAntialiasPolicy::Grayscale,
		grid::SelectWindowsTextAntialiasPolicy(conditions));
}
