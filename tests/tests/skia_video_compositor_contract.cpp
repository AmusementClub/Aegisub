#include <main.h>

#include "../../src/skia/skia_video_compositor_contract.h"

#include <thread>

namespace {
SkiaGlContextToken MakeContext(std::uintptr_t identity = 1, std::uint64_t generation = 1) {
	return { reinterpret_cast<void const *>(identity), generation };
}

SkiaVideoFrameTarget MakeTarget(std::uint64_t context_generation = 1, std::uint64_t present_generation = 1) {
	SkiaVideoFrameTarget target;
	target.context_generation = context_generation;
	target.width = 1920;
	target.height = 1080;
	target.viewport = { 0, 0, 1920, 1080 };
	target.origin = SkiaVideoTargetOrigin::BottomLeft;
	target.sample_count = 0;
	target.stencil_bits = 8;
	target.pixel_format = SkiaVideoTargetPixelFormat::Rgba8;
	target.color_space = SkiaVideoTargetColorSpace::SdrPreview;
	target.hdr_to_sdr_complete = true;
	target.present_generation = present_generation;
	return target;
}
}

TEST(skia_video_compositor_contract, valid_sdr_frame_target_is_accepted) {
	auto const validation = ValidateSkiaVideoFrameTarget(MakeTarget(), MakeContext());
	EXPECT_TRUE(validation.valid) << validation.detail;
}

TEST(skia_video_compositor_contract, ganesh_desktop_gl_floor_is_explicit) {
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl(""));
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl("1.1.0"));
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl("OpenGL ES 3.0"));
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl("2"));
	EXPECT_TRUE(SupportsSkiaGaneshDesktopGl("2.0"));
	EXPECT_TRUE(SupportsSkiaGaneshDesktopGl("  2.1 Mesa"));
	EXPECT_TRUE(SupportsSkiaGaneshDesktopGl("4.6.0 NVIDIA 591.86"));
}

TEST(skia_video_compositor_contract, frame_target_requires_exact_context_generation) {
	auto const validation = ValidateSkiaVideoFrameTarget(MakeTarget(2), MakeContext(1, 1));
	EXPECT_FALSE(validation.valid);
	EXPECT_EQ("the frame target context generation does not match the device token", validation.detail);
}

TEST(skia_video_compositor_contract, frame_target_rejects_viewport_outside_target) {
	auto target = MakeTarget();
	target.viewport = { 100, 0, 1920, 1080 };

	auto const validation = ValidateSkiaVideoFrameTarget(target, MakeContext());
	EXPECT_FALSE(validation.valid);
	EXPECT_EQ("the frame target viewport exceeds the target dimensions", validation.detail);
}

TEST(skia_video_compositor_contract, frame_target_requires_completed_sdr_preview) {
	auto target = MakeTarget();
	target.hdr_to_sdr_complete = false;

	auto const validation = ValidateSkiaVideoFrameTarget(target, MakeContext());
	EXPECT_FALSE(validation.valid);
	EXPECT_EQ("the frame target has not completed the video HDR-to-SDR pass", validation.detail);
}

TEST(skia_video_compositor_contract, device_state_binds_identity_generation_and_thread_once) {
	SkiaGlDeviceState state;
	auto const context = MakeContext(7, 3);
	auto const thread = std::this_thread::get_id();

	ASSERT_TRUE(state.BeginAccess(context, thread));
	state.MarkHealthy();
	EXPECT_TRUE(state.MatchesOwner(context, thread));
	EXPECT_EQ(SkiaGlDeviceHealth::Healthy, state.Health());
	EXPECT_TRUE(state.BeginAccess(context, thread));
}

TEST(skia_video_compositor_contract, context_identity_mismatch_is_sticky) {
	SkiaGlDeviceState state;
	auto const thread = std::this_thread::get_id();
	ASSERT_TRUE(state.BeginAccess(MakeContext(1, 4), thread));
	state.MarkHealthy();

	EXPECT_FALSE(state.BeginAccess(MakeContext(2, 4), thread));
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
	EXPECT_EQ(SkiaGlDeviceFailure::ContextIdentityMismatch, state.LastFailure());
	EXPECT_FALSE(state.BeginAccess(MakeContext(1, 4), thread));
	EXPECT_EQ(SkiaGlDeviceFailure::ContextIdentityMismatch, state.LastFailure());
}

TEST(skia_video_compositor_contract, context_generation_mismatch_is_sticky) {
	SkiaGlDeviceState state;
	auto const thread = std::this_thread::get_id();
	ASSERT_TRUE(state.BeginAccess(MakeContext(1, 8), thread));
	state.MarkHealthy();

	EXPECT_FALSE(state.BeginAccess(MakeContext(1, 9), thread));
	EXPECT_EQ(SkiaGlDeviceFailure::ContextGenerationMismatch, state.LastFailure());
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
}

TEST(skia_video_compositor_contract, wrong_thread_is_sticky) {
	SkiaGlDeviceState state;
	auto const context = MakeContext();
	ASSERT_TRUE(state.BeginAccess(context, std::this_thread::get_id()));
	state.MarkHealthy();

	bool other_thread_result = true;
	std::thread other([&] {
		other_thread_result = state.BeginAccess(context, std::this_thread::get_id());
	});
	other.join();

	EXPECT_FALSE(other_thread_result);
	EXPECT_EQ(SkiaGlDeviceFailure::WrongThread, state.LastFailure());
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
}

TEST(skia_video_compositor_contract, first_failure_reason_is_preserved) {
	SkiaGlDeviceState state;
	state.Fail(SkiaGlDeviceFailure::FrameBeginInjected, "first");
	state.Fail(SkiaGlDeviceFailure::FlushInjected, "second");

	EXPECT_EQ(SkiaGlDeviceFailure::FrameBeginInjected, state.LastFailure());
	EXPECT_EQ("first", state.LastFailureDetail());
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
}

TEST(skia_video_compositor_contract, abandoned_state_cannot_be_rebound) {
	SkiaGlDeviceState state;
	ASSERT_TRUE(state.BeginAccess(MakeContext(), std::this_thread::get_id()));
	state.MarkHealthy();
	state.MarkAbandoned();

	EXPECT_FALSE(state.BeginAccess(MakeContext(), std::this_thread::get_id()));
	EXPECT_EQ(SkiaGlDeviceHealth::Abandoned, state.Health());
}

TEST(skia_video_compositor_contract, failure_injection_parser_is_exact) {
	EXPECT_EQ(SkiaVideoFailureInjection::None, ParseSkiaVideoFailureInjection(""));
	EXPECT_EQ(SkiaVideoFailureInjection::None, ParseSkiaVideoFailureInjection("none"));
	EXPECT_EQ(SkiaVideoFailureInjection::ContextInitialization, ParseSkiaVideoFailureInjection("context-init"));
	EXPECT_EQ(SkiaVideoFailureInjection::FrameBegin, ParseSkiaVideoFailureInjection("frame-begin"));
	EXPECT_EQ(SkiaVideoFailureInjection::FlushSubmit, ParseSkiaVideoFailureInjection("flush-submit"));
	EXPECT_EQ(SkiaVideoFailureInjection::Unsupported, ParseSkiaVideoFailureInjection("flush"));
}
