#include "skia_video_compositor_contract.h"

#include <utility>

char const *ToString(SkiaGlDeviceHealth health) noexcept {
	switch (health) {
		case SkiaGlDeviceHealth::Uninitialized: return "uninitialized";
		case SkiaGlDeviceHealth::Healthy: return "healthy";
		case SkiaGlDeviceHealth::Unhealthy: return "unhealthy";
		case SkiaGlDeviceHealth::Abandoned: return "abandoned";
	}
	return "unknown";
}

char const *ToString(SkiaGlDeviceFailure failure) noexcept {
	switch (failure) {
		case SkiaGlDeviceFailure::None: return "none";
		case SkiaGlDeviceFailure::InvalidContextIdentity: return "invalid-context-identity";
		case SkiaGlDeviceFailure::InvalidContextGeneration: return "invalid-context-generation";
		case SkiaGlDeviceFailure::ContextActivationFailed: return "context-activation-failed";
		case SkiaGlDeviceFailure::ContextIdentityMismatch: return "context-identity-mismatch";
		case SkiaGlDeviceFailure::ContextGenerationMismatch: return "context-generation-mismatch";
		case SkiaGlDeviceFailure::WrongThread: return "wrong-thread";
		case SkiaGlDeviceFailure::ContextInitializationInjected: return "context-initialization-injected";
		case SkiaGlDeviceFailure::GlVersionUnsupported: return "gl-version-unsupported";
		case SkiaGlDeviceFailure::GlInterfaceUnavailable: return "gl-interface-unavailable";
		case SkiaGlDeviceFailure::GaneshContextUnavailable: return "ganesh-context-unavailable";
		case SkiaGlDeviceFailure::GaneshContextAbandoned: return "ganesh-context-abandoned";
		case SkiaGlDeviceFailure::GaneshOutOfMemory: return "ganesh-out-of-memory";
		case SkiaGlDeviceFailure::InvalidFrameTarget: return "invalid-frame-target";
		case SkiaGlDeviceFailure::StalePresentGeneration: return "stale-present-generation";
		case SkiaGlDeviceFailure::FrameAlreadyOpen: return "frame-already-open";
		case SkiaGlDeviceFailure::FrameNotOpen: return "frame-not-open";
		case SkiaGlDeviceFailure::FrameBeginInjected: return "frame-begin-injected";
		case SkiaGlDeviceFailure::SurfaceAllocationFailed: return "surface-allocation-failed";
		case SkiaGlDeviceFailure::SurfaceAcquisitionFailed: return "surface-acquisition-failed";
		case SkiaGlDeviceFailure::FlushInjected: return "flush-injected";
		case SkiaGlDeviceFailure::UnsupportedFailureInjection: return "unsupported-failure-injection";
	}
	return "unknown";
}

bool SupportsSkiaGaneshDesktopGl(std::string_view version) noexcept {
	std::size_t position = 0;
	while (position < version.size() && (version[position] == ' ' || version[position] == '\t'))
		++position;
	if (position == version.size() || version[position] < '0' || version[position] > '9')
		return false;

	unsigned int major = 0;
	while (position < version.size() && version[position] >= '0' && version[position] <= '9') {
		major = major * 10 + static_cast<unsigned int>(version[position] - '0');
		if (major > 99)
			return false;
		++position;
	}
	if (position == version.size() || version[position] != '.')
		return false;
	++position;
	if (position == version.size() || version[position] < '0' || version[position] > '9')
		return false;

	// The minor component only needs to be syntactically valid. Every desktop
	// GL version with a major component of two or newer satisfies Ganesh's floor.
	while (position < version.size() && version[position] >= '0' && version[position] <= '9')
		++position;
	return major >= 2;
}

bool SkiaGlDeviceState::BeginAccess(SkiaGlContextToken token, std::thread::id current_thread) {
	if (health == SkiaGlDeviceHealth::Unhealthy || health == SkiaGlDeviceHealth::Abandoned)
		return false;

	if (!token.identity) {
		Fail(SkiaGlDeviceFailure::InvalidContextIdentity, "the context identity is null");
		return false;
	}
	if (!token.generation) {
		Fail(SkiaGlDeviceFailure::InvalidContextGeneration, "the context generation is zero");
		return false;
	}

	if (!owner_bound) {
		owner_bound = true;
		owner = token;
		owner_thread = current_thread;
		return true;
	}

	if (owner.identity != token.identity) {
		Fail(SkiaGlDeviceFailure::ContextIdentityMismatch, "the wxGLContext identity changed without rebuilding the device");
		return false;
	}
	if (owner.generation != token.generation) {
		Fail(SkiaGlDeviceFailure::ContextGenerationMismatch, "the wxGLContext generation changed without rebuilding the device");
		return false;
	}
	if (owner_thread != current_thread) {
		Fail(SkiaGlDeviceFailure::WrongThread, "the device was accessed from a thread other than its owning UI thread");
		return false;
	}
	return true;
}

bool SkiaGlDeviceState::MatchesOwner(
	SkiaGlContextToken token,
	std::thread::id current_thread) const noexcept {
	return owner_bound
		&& owner.identity == token.identity
		&& owner.generation == token.generation
		&& owner_thread == current_thread;
}

void SkiaGlDeviceState::MarkHealthy() noexcept {
	if (health == SkiaGlDeviceHealth::Uninitialized && last_failure == SkiaGlDeviceFailure::None)
		health = SkiaGlDeviceHealth::Healthy;
}

void SkiaGlDeviceState::Fail(SkiaGlDeviceFailure failure, std::string detail) {
	if (last_failure == SkiaGlDeviceFailure::None) {
		last_failure = failure;
		last_failure_detail = std::move(detail);
	}
	if (health != SkiaGlDeviceHealth::Abandoned)
		health = SkiaGlDeviceHealth::Unhealthy;
}

void SkiaGlDeviceState::MarkAbandoned() noexcept {
	health = SkiaGlDeviceHealth::Abandoned;
}

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
