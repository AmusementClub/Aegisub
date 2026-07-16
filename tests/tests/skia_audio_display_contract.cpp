#include "../../src/skia/audio/skia_audio_display_contract.h"

#include <gtest/gtest.h>

namespace audio = aegisub::skia::audio;

TEST(skia_audio_display_contract, runtime_opt_in_is_default_off) {
	EXPECT_FALSE(audio::ParseRuntimeOptIn(nullptr));
	EXPECT_FALSE(audio::ParseRuntimeOptIn(""));
	EXPECT_FALSE(audio::ParseRuntimeOptIn("0"));
	EXPECT_FALSE(audio::ParseRuntimeOptIn("false"));
	EXPECT_FALSE(audio::ParseRuntimeOptIn("No"));
	EXPECT_TRUE(audio::ParseRuntimeOptIn("1"));
	EXPECT_TRUE(audio::ParseRuntimeOptIn("true"));
	EXPECT_TRUE(audio::ParseRuntimeOptIn("yes"));
}

TEST(skia_audio_display_contract, widget_creation_requires_runtime_opt_in_and_presenter_availability) {
	EXPECT_FALSE(audio::ShouldCreateSkiaWidget(false, false));
	EXPECT_FALSE(audio::ShouldCreateSkiaWidget(true, false));
	EXPECT_FALSE(audio::ShouldCreateSkiaWidget(false, true));
	EXPECT_TRUE(audio::ShouldCreateSkiaWidget(true, true));
}

TEST(skia_audio_display_contract, desktop_gl_floor_is_explicit) {
	EXPECT_FALSE(audio::IsDesktopGlAtLeast(1, 1, 2, 0));
	EXPECT_TRUE(audio::IsDesktopGlAtLeast(2, 0, 2, 0));
	EXPECT_TRUE(audio::IsDesktopGlAtLeast(4, 6, 2, 0));
}

TEST(skia_audio_display_contract, software_and_remote_gl_renderers_are_rejected) {
	EXPECT_TRUE(audio::IsSoftwareLikeGlRenderer("Microsoft Corporation", "GDI Generic"));
	EXPECT_TRUE(audio::IsSoftwareLikeGlRenderer("Mesa", "llvmpipe (LLVM 20.1.0, 256 bits)"));
	EXPECT_TRUE(audio::IsSoftwareLikeGlRenderer("Google Inc.", "ANGLE (Microsoft Basic Render Driver)"));
	EXPECT_FALSE(audio::IsSoftwareLikeGlRenderer("NVIDIA Corporation", "NVIDIA GeForce RTX 2080 Ti"));
	EXPECT_FALSE(audio::IsSoftwareLikeGlRenderer("Intel", "Intel(R) UHD Graphics"));
}

TEST(skia_audio_display_contract, audio_failure_injection_parser_is_exact) {
	EXPECT_EQ(audio::FailureInjection::None, audio::ParseFailureInjection(""));
	EXPECT_EQ(audio::FailureInjection::None, audio::ParseFailureInjection("none"));
	EXPECT_EQ(audio::FailureInjection::ContextInitialization, audio::ParseFailureInjection("context-init"));
	EXPECT_EQ(audio::FailureInjection::FrameBegin, audio::ParseFailureInjection("frame-begin"));
	EXPECT_EQ(audio::FailureInjection::FlushSubmit, audio::ParseFailureInjection("flush-submit"));
	EXPECT_EQ(audio::FailureInjection::Unsupported, audio::ParseFailureInjection("flush"));
}

TEST(skia_audio_display_contract, frame_target_and_surface_key_require_exact_generation_and_size) {
	audio::FrameTarget target;
	target.context_generation = 7;
	target.width = 1920;
	target.height = 240;
	target.stencil_bits = 8;

	auto const valid = audio::ValidateFrameTarget(target, 7);
	EXPECT_TRUE(valid.valid) << valid.detail;
	EXPECT_FALSE(audio::ValidateFrameTarget(target, 8).valid);

	auto const first = audio::MakeSurfaceKey(target);
	EXPECT_EQ(first, audio::MakeSurfaceKey(target));
	target.width = 3840;
	EXPECT_NE(first, audio::MakeSurfaceKey(target));
}

TEST(skia_audio_display_contract, selection_requires_runtime_context_gl_and_hardware_renderer) {
	audio::Capabilities capabilities { true, 4, 6, false };
	EXPECT_EQ(audio::Backend::Wx, audio::SelectBackend(false, capabilities).backend);

	capabilities.context_available = false;
	EXPECT_EQ(audio::SelectionReason::ContextUnavailable, audio::SelectBackend(true, capabilities).reason);

	capabilities = { true, 1, 1, false };
	EXPECT_EQ(audio::SelectionReason::DesktopGlTooOld, audio::SelectBackend(true, capabilities).reason);

	capabilities = { true, 4, 6, true };
	EXPECT_EQ(audio::SelectionReason::SoftwareLikeRenderer, audio::SelectBackend(true, capabilities).reason);

	capabilities.software_like_renderer = false;
	auto const selected = audio::SelectBackend(true, capabilities);
	EXPECT_EQ(audio::Backend::Skia, selected.backend);
	EXPECT_EQ(audio::SelectionReason::SkiaAvailable, selected.reason);
}

TEST(skia_audio_display_contract, runtime_failure_prompts_only_after_content_was_presented) {
	EXPECT_EQ(
		audio::RuntimeFallbackDisposition::Automatic,
		audio::PlanRuntimeFallback(false));
	EXPECT_EQ(
		audio::RuntimeFallbackDisposition::Confirm,
		audio::PlanRuntimeFallback(true));
}

TEST(skia_audio_display_contract, cursor_update_is_overlay_only) {
	audio::Revisions current;
	auto const plan = audio::PlanTransition(current, audio::Change::Cursor);
	EXPECT_EQ(current.cursor + 1, plan.next.cursor);
	EXPECT_EQ(current.content, plan.next.content);
	EXPECT_EQ(current.analysis, plan.next.analysis);
	EXPECT_EQ(audio::Layer::Cursor, plan.dirty_layers);
	EXPECT_FALSE(plan.invalidate_analysis_tiles);
	EXPECT_FALSE(plan.invalidate_gpu_content_tiles);
	EXPECT_FALSE(plan.request_visible_tiles);
}

TEST(skia_audio_display_contract, marker_update_does_not_touch_content) {
	audio::Revisions current;
	auto const plan = audio::PlanTransition(current, audio::Change::Marker);
	EXPECT_EQ(current.marker + 1, plan.next.marker);
	EXPECT_EQ(current.content, plan.next.content);
	EXPECT_EQ(current.analysis, plan.next.analysis);
	EXPECT_EQ(audio::Layer::Marker, plan.dirty_layers);
}

TEST(skia_audio_display_contract, scroll_recomposes_without_invalidating_tiles) {
	audio::Revisions current;
	auto const plan = audio::PlanTransition(current, audio::Change::Scroll);
	EXPECT_EQ(current.viewport + 1, plan.next.viewport);
	EXPECT_EQ(current.analysis, plan.next.analysis);
	EXPECT_EQ(current.content, plan.next.content);
	EXPECT_EQ(audio::Layer::All, plan.dirty_layers);
	EXPECT_TRUE(plan.request_visible_tiles);
	EXPECT_FALSE(plan.invalidate_analysis_tiles);
	EXPECT_FALSE(plan.invalidate_gpu_content_tiles);
}

TEST(skia_audio_display_contract, presentation_changes_do_not_reanalyze_audio) {
	for (auto const change : { audio::Change::Amplitude, audio::Change::Palette, audio::Change::Style }) {
		audio::Revisions current;
		auto const plan = audio::PlanTransition(current, change);
		EXPECT_EQ(current.analysis, plan.next.analysis);
		EXPECT_FALSE(plan.invalidate_analysis_tiles);
		EXPECT_FALSE(plan.invalidate_gpu_content_tiles);
	}
}

TEST(skia_audio_display_contract, zoom_invalidates_analysis_and_content_tiles) {
	audio::Revisions current;
	auto const plan = audio::PlanTransition(current, audio::Change::Zoom);
	EXPECT_EQ(current.analysis + 1, plan.next.analysis);
	EXPECT_EQ(current.viewport + 1, plan.next.viewport);
	EXPECT_EQ(current.content + 1, plan.next.content);
	EXPECT_TRUE(plan.invalidate_analysis_tiles);
	EXPECT_TRUE(plan.invalidate_gpu_content_tiles);
	EXPECT_TRUE(plan.request_visible_tiles);
}

TEST(skia_audio_display_contract, provider_replacement_invalidates_every_layer) {
	audio::Revisions current;
	auto const plan = audio::PlanTransition(current, audio::Change::Provider);
	EXPECT_EQ(audio::Layer::All, plan.dirty_layers);
	EXPECT_NE(current.provider, plan.next.provider);
	EXPECT_NE(current.analysis, plan.next.analysis);
	EXPECT_NE(current.viewport, plan.next.viewport);
	EXPECT_TRUE(plan.invalidate_analysis_tiles);
	EXPECT_TRUE(plan.invalidate_gpu_content_tiles);
	EXPECT_TRUE(plan.invalidate_text_cache);
}

TEST(skia_audio_display_contract, resize_recreates_surface_without_reanalysis) {
	audio::Revisions current;
	auto const plan = audio::PlanTransition(current, audio::Change::Resize);
	EXPECT_TRUE(plan.recreate_surface);
	EXPECT_TRUE(plan.request_visible_tiles);
	EXPECT_EQ(current.analysis, plan.next.analysis);
	EXPECT_FALSE(plan.invalidate_analysis_tiles);
}

TEST(skia_audio_display_contract, failure_is_sticky_and_preserves_first_reason) {
	audio::FailureState state;
	EXPECT_TRUE(state.CanUseSkia());
	EXPECT_FALSE(state.MarkFailed(audio::Failure::None));
	EXPECT_TRUE(state.MarkFailed(audio::Failure::FrameBegin));
	EXPECT_FALSE(state.CanUseSkia());
	EXPECT_FALSE(state.MarkFailed(audio::Failure::FlushSubmit));
	EXPECT_EQ(audio::Failure::FrameBegin, state.FirstFailure());
}
