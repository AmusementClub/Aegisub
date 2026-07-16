#include "skia_video_compositor_contract.h"

SkiaVideoFailureInjection ParseSkiaVideoFailureInjection(std::string_view value) noexcept {
	if (value.empty() || value == "none")
		return SkiaVideoFailureInjection::None;
	if (value == "context-init")
		return SkiaVideoFailureInjection::ContextInitialization;
	if (value == "frame-begin")
		return SkiaVideoFailureInjection::FrameBegin;
	if (value == "flush-submit")
		return SkiaVideoFailureInjection::FlushSubmit;
	return SkiaVideoFailureInjection::Unsupported;
}

char const *ToString(SkiaVideoFailureInjection injection) noexcept {
	switch (injection) {
		case SkiaVideoFailureInjection::None: return "none";
		case SkiaVideoFailureInjection::ContextInitialization: return "context-init";
		case SkiaVideoFailureInjection::FrameBegin: return "frame-begin";
		case SkiaVideoFailureInjection::FlushSubmit: return "flush-submit";
		case SkiaVideoFailureInjection::Unsupported: return "unsupported";
	}
	return "unknown";
}

SkiaVideoFrameTargetValidation ValidateSkiaVideoFrameTarget(
	SkiaVideoFrameTarget const& target,
	SkiaGlContextToken context) {
	if (!context.identity)
		return { false, "the context identity is null" };
	if (!context.generation)
		return { false, "the context generation is zero" };
	if (target.context_generation != context.generation)
		return { false, "the frame target context generation does not match the device token" };
	if (target.width <= 0 || target.height <= 0)
		return { false, "the frame target dimensions are not positive" };
	if (target.viewport.x < 0 || target.viewport.y < 0
		|| target.viewport.width <= 0 || target.viewport.height <= 0) {
		return { false, "the frame target viewport is invalid" };
	}
	if (target.viewport.x > target.width - target.viewport.width
		|| target.viewport.y > target.height - target.viewport.height) {
		return { false, "the frame target viewport exceeds the target dimensions" };
	}
	if (target.origin == SkiaVideoTargetOrigin::Unknown)
		return { false, "the frame target origin is unknown" };
	if (target.sample_count < 0 || target.stencil_bits < 0)
		return { false, "the frame target sample or stencil count is negative" };
	if (target.pixel_format == SkiaVideoTargetPixelFormat::Unknown)
		return { false, "the frame target pixel format is unknown" };
	if (target.color_space == SkiaVideoTargetColorSpace::Unknown)
		return { false, "the frame target color space is unknown" };
	if (!target.hdr_to_sdr_complete)
		return { false, "the frame target has not completed the video HDR-to-SDR pass" };
	if (!target.present_generation)
		return { false, "the frame target present generation is zero" };
	return { true, {} };
}
