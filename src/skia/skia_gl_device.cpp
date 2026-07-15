#include "skia/skia_gl_device.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <include/core/SkRefCnt.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>

#include <thread>
#include <utility>

namespace {
std::string ReadGlString(GLenum name) {
	auto const *value = glGetString(name);
	return value ? reinterpret_cast<char const *>(value) : std::string{};
}
}

struct SkiaGlDevice::Impl {
	explicit Impl(SkiaVideoFailureInjection failure_injection)
	: failure_injection(failure_injection) {
	}

	SkiaVideoFailureInjection failure_injection = SkiaVideoFailureInjection::None;
	SkiaGlDeviceState state;
	sk_sp<const GrGLInterface> gl_interface;
	sk_sp<GrDirectContext> context;
	std::string gl_vendor;
	std::string gl_renderer;
	std::string gl_version;

	void TripFailure(SkiaGlDeviceFailure failure, std::string detail) noexcept {
		state.Fail(failure, std::move(detail));
		if (context && !context->abandoned())
			context->abandonContext();
	}

	bool EnsureCurrent(SkiaGlContextToken token) {
		if (!state.BeginAccess(token, std::this_thread::get_id()))
			return false;

		if (context) {
			if (context->abandoned()) {
				TripFailure(SkiaGlDeviceFailure::GaneshContextAbandoned, "the Ganesh context is abandoned");
				return false;
			}
			return true;
		}

		if (failure_injection == SkiaVideoFailureInjection::ContextInitialization) {
			TripFailure(
				SkiaGlDeviceFailure::ContextInitializationInjected,
				"AEGISUB_SKIA_VIDEO_FAILURE_INJECTION requested context-init");
			return false;
		}

		gl_interface = GrGLMakeNativeInterface();
		if (!gl_interface) {
			TripFailure(SkiaGlDeviceFailure::GlInterfaceUnavailable, "GrGLMakeNativeInterface returned null");
			return false;
		}

		context = GrDirectContexts::MakeGL(gl_interface);
		if (!context) {
			TripFailure(SkiaGlDeviceFailure::GaneshContextUnavailable, "GrDirectContexts::MakeGL returned null");
			return false;
		}

		gl_vendor = ReadGlString(GL_VENDOR);
		gl_renderer = ReadGlString(GL_RENDERER);
		gl_version = ReadGlString(GL_VERSION);
		state.MarkHealthy();
		return true;
	}
};

SkiaGlDevice::SkiaGlDevice(SkiaVideoFailureInjection failure_injection)
: impl(std::make_unique<Impl>(failure_injection)) {
}

SkiaGlDevice::~SkiaGlDevice() {
	Abandon();
}

bool SkiaGlDevice::BeginExternalFrame(SkiaGlContextToken token) {
	if (!impl->EnsureCurrent(token))
		return false;

	impl->context->resetContext();
	if (impl->context->abandoned()) {
		impl->TripFailure(SkiaGlDeviceFailure::GaneshContextAbandoned, "the Ganesh context was abandoned while entering the frame");
		return false;
	}
	return true;
}

bool SkiaGlDevice::FlushAndSubmit(SkiaGlContextToken token) {
	if (!impl->state.BeginAccess(token, std::this_thread::get_id()) || !impl->context)
		return false;
	if (impl->failure_injection == SkiaVideoFailureInjection::FlushSubmit) {
		impl->TripFailure(
			SkiaGlDeviceFailure::FlushInjected,
			"AEGISUB_SKIA_VIDEO_FAILURE_INJECTION requested flush-submit");
		return false;
	}

	impl->context->flushAndSubmit();
	if (impl->context->abandoned()) {
		impl->TripFailure(SkiaGlDeviceFailure::GaneshContextAbandoned, "the Ganesh context was abandoned during flushAndSubmit");
		return false;
	}
	if (impl->context->oomed()) {
		impl->TripFailure(SkiaGlDeviceFailure::GaneshOutOfMemory, "the Ganesh context reported an out-of-memory condition");
		return false;
	}
	return true;
}

void SkiaGlDevice::ResetTextureBindingsForExternalUse(SkiaGlContextToken token) noexcept {
	if (!impl->context
		|| impl->context->abandoned()
		|| !impl->state.MatchesOwner(token, std::this_thread::get_id())) {
		return;
	}
	impl->context->resetGLTextureBindings();
}

void SkiaGlDevice::Fail(
	SkiaGlContextToken token,
	SkiaGlDeviceFailure failure,
	std::string detail) noexcept {
	if (!impl->state.BeginAccess(token, std::this_thread::get_id()))
		return;
	impl->TripFailure(failure, std::move(detail));
}

void SkiaGlDevice::ReleaseResourcesAndAbandon(SkiaGlContextToken token) noexcept {
	if (impl->context) {
		if (impl->state.MatchesOwner(token, std::this_thread::get_id())
			&& !impl->context->abandoned()) {
			impl->context->releaseResourcesAndAbandonContext();
		}
		else if (!impl->context->abandoned()) {
			impl->context->abandonContext();
		}
		impl->context.reset();
	}
	impl->gl_interface.reset();
	impl->state.MarkAbandoned();
}

void SkiaGlDevice::Abandon() noexcept {
	if (!impl)
		return;
	if (impl->context) {
		if (!impl->context->abandoned())
			impl->context->abandonContext();
		impl->context.reset();
	}
	impl->gl_interface.reset();
	impl->state.MarkAbandoned();
}

GrDirectContext *SkiaGlDevice::Get() const noexcept {
	return impl->state.Health() == SkiaGlDeviceHealth::Healthy
		&& impl->context
		&& !impl->context->abandoned()
		? impl->context.get()
		: nullptr;
}

SkiaGlDeviceHealth SkiaGlDevice::Health() const noexcept {
	return impl->state.Health();
}

SkiaGlDeviceFailure SkiaGlDevice::LastFailure() const noexcept {
	return impl->state.LastFailure();
}

std::string const& SkiaGlDevice::LastFailureDetail() const noexcept {
	return impl->state.LastFailureDetail();
}

std::string const& SkiaGlDevice::GlVendor() const noexcept {
	return impl->gl_vendor;
}

std::string const& SkiaGlDevice::GlRenderer() const noexcept {
	return impl->gl_renderer;
}

std::string const& SkiaGlDevice::GlVersion() const noexcept {
	return impl->gl_version;
}
