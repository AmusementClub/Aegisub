#include <main.h>

#include "../../src/skia/skia_video_compositor_contract.h"

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

TEST(skia_video_compositor_contract, failure_injection_parser_is_exact) {
	EXPECT_EQ(SkiaVideoFailureInjection::None, ParseSkiaVideoFailureInjection(""));
	EXPECT_EQ(SkiaVideoFailureInjection::None, ParseSkiaVideoFailureInjection("none"));
	EXPECT_EQ(SkiaVideoFailureInjection::ContextInitialization, ParseSkiaVideoFailureInjection("context-init"));
	EXPECT_EQ(SkiaVideoFailureInjection::FrameBegin, ParseSkiaVideoFailureInjection("frame-begin"));
	EXPECT_EQ(SkiaVideoFailureInjection::FlushSubmit, ParseSkiaVideoFailureInjection("flush-submit"));
	EXPECT_EQ(SkiaVideoFailureInjection::Unsupported, ParseSkiaVideoFailureInjection("flush"));
}
