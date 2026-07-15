#include "skia/skia_video_compositor.h"

#include <sstream>
#include <thread>
#include <utility>

SkiaVideoCompositor::SkiaVideoCompositor(SkiaVideoFailureInjection failure_injection)
: failure_injection(failure_injection)
, device(failure_injection) {
}

SkiaVideoCompositor::~SkiaVideoCompositor() = default;

bool SkiaVideoCompositor::BeginFrame(
	SkiaGlContextToken context,
	SkiaVideoFrameTarget const& target) {
	last_context = context;
	if (frame_open) {
		device.Fail(context, SkiaGlDeviceFailure::FrameAlreadyOpen, "BeginFrame was called before the previous frame was closed");
		return false;
	}
	if (failure_injection == SkiaVideoFailureInjection::Unsupported) {
		device.Fail(
			context,
			SkiaGlDeviceFailure::UnsupportedFailureInjection,
			"AEGISUB_SKIA_VIDEO_FAILURE_INJECTION contains an unsupported value");
		return false;
	}

	auto validation = ValidateSkiaVideoFrameTarget(target, context);
	if (!validation.valid) {
		device.Fail(context, SkiaGlDeviceFailure::InvalidFrameTarget, std::move(validation.detail));
		return false;
	}
	if (last_present_generation && target.present_generation <= last_present_generation) {
		device.Fail(
			context,
			SkiaGlDeviceFailure::StalePresentGeneration,
			"the frame target present generation did not advance monotonically");
		return false;
	}
	if (failure_injection == SkiaVideoFailureInjection::FrameBegin) {
		device.Fail(
			context,
			SkiaGlDeviceFailure::FrameBeginInjected,
			"AEGISUB_SKIA_VIDEO_FAILURE_INJECTION requested frame-begin");
		return false;
	}
	if (!device.BeginExternalFrame(context))
		return false;

	frame_open = true;
	active_context = context;
	last_present_generation = target.present_generation;
	return true;
}

bool SkiaVideoCompositor::FinishFrame(SkiaGlContextToken context, bool submit) {
	last_context = context;
	if (!frame_open) {
		device.Fail(context, SkiaGlDeviceFailure::FrameNotOpen, "FinishFrame was called without a matching BeginFrame");
		return false;
	}
	if (active_context.identity != context.identity
		|| active_context.generation != context.generation) {
		device.Fail(context, SkiaGlDeviceFailure::ContextIdentityMismatch, "FinishFrame used a different context token than BeginFrame");
		frame_open = false;
		active_context = {};
		return false;
	}

	bool const succeeded = !submit || device.FlushAndSubmit(context);
	device.ResetTextureBindingsForExternalUse(context);
	frame_open = false;
	active_context = {};
	return succeeded;
}

bool SkiaVideoCompositor::ProbeFrame(
	SkiaGlContextToken context,
	SkiaVideoFrameTarget const& target) {
	if (!BeginFrame(context, target))
		return false;

	// A flush injection is allowed to exercise the failure boundary without
	// recording any draw commands. Normal probes never submit GPU work.
	return FinishFrame(context, failure_injection == SkiaVideoFailureInjection::FlushSubmit);
}

void SkiaVideoCompositor::FailFrame(
	SkiaGlContextToken context,
	SkiaGlDeviceFailure failure,
	std::string detail) noexcept {
	last_context = context;
	device.Fail(context, failure, std::move(detail));
	device.ResetTextureBindingsForExternalUse(context);
	frame_open = false;
	active_context = {};
}

void SkiaVideoCompositor::NotifyContextActivationFailure(SkiaGlContextToken context) noexcept {
	last_context = context;
	device.Fail(
		context,
		SkiaGlDeviceFailure::ContextActivationFailed,
		"wxGLCanvas::SetCurrent failed for the Skia-owned context token");
}

void SkiaVideoCompositor::Release(SkiaGlContextToken context) noexcept {
	last_context = context;
	if (frame_open)
		device.ResetTextureBindingsForExternalUse(context);
	frame_open = false;
	active_context = {};
	device.ReleaseResourcesAndAbandon(context);
}

std::string SkiaVideoCompositor::TakeFailureLogMessage() {
	if (failure_logged || device.LastFailure() == SkiaGlDeviceFailure::None)
		return {};
	failure_logged = true;

	std::ostringstream message;
	message
		<< "Skia video compositor is disabled for this GL context: context="
		<< last_context.identity
		<< ", generation=" << last_context.generation
		<< ", health=" << ToString(device.Health())
		<< ", failure=" << ToString(device.LastFailure());
	if (!device.LastFailureDetail().empty())
		message << ", detail=" << device.LastFailureDetail();
	if (!device.GlVendor().empty())
		message << ", GL_VENDOR=" << device.GlVendor();
	if (!device.GlRenderer().empty())
		message << ", GL_RENDERER=" << device.GlRenderer();
	if (!device.GlVersion().empty())
		message << ", GL_VERSION=" << device.GlVersion();
	return message.str();
}
