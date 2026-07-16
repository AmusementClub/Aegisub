#include "skia_audio_display.h"

#include "skia_audio_content_worker.h"
#include "skia_audio_presenter.h"

#include "../../include/aegisub/context.h"
#include "../../project.h"

#include <libaegisub/signal.h>

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

#include <wx/dcclient.h>
#include <wx/thread.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace aegisub::skia::audio {
namespace {

wxDEFINE_EVENT(EVT_SKIA_AUDIO_CONTENT_READY, wxThreadEvent);

#if wxCHECK_VERSION(3, 1, 1)
int gl_attributes[] = {
	WX_GL_RGBA,
	WX_GL_DOUBLEBUFFER,
	WX_GL_STENCIL_SIZE, 8,
	WX_GL_BUFFER_SIZE, 24,
	WX_GL_MIN_ALPHA, 8,
	0,
};
#else
int gl_attributes[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

}

struct SkiaAudioDisplay::Impl {
	Impl(
		SkiaAudioDisplay *owner,
		agi::Context *project_context,
		FailureInjection failure_injection,
		FailureCallback failure_callback)
	: presenter(std::make_unique<Presenter>(failure_injection))
	, content_worker([owner](ContentGeneration) {
		wxQueueEvent(owner, new wxThreadEvent(EVT_SKIA_AUDIO_CONTENT_READY));
	})
	, failure_callback(std::move(failure_callback)) {
		this->project_context = project_context;
	}

	std::unique_ptr<wxGLContext> context;
	std::unique_ptr<Presenter> presenter;
	ContentWorker content_worker;
	agi::signal::Connection audio_open_connection;
	agi::Context *project_context = nullptr;
	FailureCallback failure_callback;
	std::uint64_t context_generation = 0;
	bool fallback_requested = false;

	SkiaGlContextToken ContextToken() const noexcept {
		return { context.get(), context_generation };
	}
};

SkiaAudioDisplay::SkiaAudioDisplay(
	wxWindow *parent,
	agi::Context *context,
	FailureInjection failure_injection,
	FailureCallback failure_callback)
: wxGLCanvas(parent, wxID_ANY, gl_attributes, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
, impl(std::make_unique<Impl>(this, context, failure_injection, std::move(failure_callback)))
{
	impl->project_context = context;
	impl->audio_open_connection = context->GetCore().project->AddAudioProviderListener(
		&SkiaAudioDisplay::OnAudioOpen,
		this);
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Bind(wxEVT_PAINT, &SkiaAudioDisplay::OnPaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, &SkiaAudioDisplay::OnEraseBackground, this);
	Bind(wxEVT_SIZE, &SkiaAudioDisplay::OnSize, this);
	Bind(EVT_SKIA_AUDIO_CONTENT_READY, &SkiaAudioDisplay::OnContentReady, this);
}

SkiaAudioDisplay::~SkiaAudioDisplay() {
	if (!impl || !impl->presenter)
		return;
	if (impl->context && impl->context->IsOK() && SetCurrent(*impl->context))
		impl->presenter->Release(impl->ContextToken());
	else
		impl->presenter->Abandon();
	impl->presenter.reset();
	impl->context.reset();
}

void SkiaAudioDisplay::OnPaint(wxPaintEvent&) try {
	wxPaintDC paint_dc(this);
	if (impl->fallback_requested)
		return;

	auto const logical_size = GetClientSize();
	if (logical_size.GetWidth() <= 0 || logical_size.GetHeight() <= 0)
		return;

	if (!impl->context) {
		impl->context = std::make_unique<wxGLContext>(this);
		++impl->context_generation;
		if (!impl->context_generation)
			++impl->context_generation;
	}

	auto const context = impl->ContextToken();
	if (!impl->context->IsOK() || !SetCurrent(*impl->context)) {
		impl->presenter->Fail(
			context,
			SkiaGlDeviceFailure::ContextActivationFailed,
			"wxGLCanvas could not activate the Audio Display context");
		RequestFallback(impl->presenter->TakeFailureLogMessage());
		return;
	}

	double const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	FrameTarget target;
	target.context_generation = context.generation;
	target.width = std::max(1, static_cast<int>(std::lround(logical_size.GetWidth() * scale)));
	target.height = std::max(1, static_cast<int>(std::lround(logical_size.GetHeight() * scale)));
	target.sample_count = 0;
	GLint stencil_bits = 0;
	glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
	target.stencil_bits = std::max(0, static_cast<int>(stencil_bits));
	target.framebuffer_id = 0;
	target.bottom_left_origin = true;

	if (!impl->presenter->RenderDiagnosticFrame(context, target)) {
		RequestFallback(impl->presenter->TakeFailureLogMessage());
		return;
	}
	if (!SwapBuffers()) {
		impl->presenter->Fail(context, SkiaGlDeviceFailure::SwapBuffersFailed, "wxGLCanvas::SwapBuffers failed");
		RequestFallback(impl->presenter->TakeFailureLogMessage());
	}
}
catch (std::exception const& err) {
	if (impl->presenter && impl->context) {
		impl->presenter->Fail(
			impl->ContextToken(),
			SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
			err.what());
		RequestFallback(impl->presenter->TakeFailureLogMessage());
	}
	else {
		RequestFallback(std::string("Skia Audio Display initialization failed: ") + err.what());
	}
}
catch (...) {
	RequestFallback("an unknown exception escaped Skia Audio Display paint");
}

void SkiaAudioDisplay::OnEraseBackground(wxEraseEvent&) {
}

void SkiaAudioDisplay::OnSize(wxSizeEvent& event) {
	Refresh(false);
	event.Skip();
}

void SkiaAudioDisplay::OnContentReady(wxThreadEvent&) {
	Refresh(false);
}

void SkiaAudioDisplay::OnAudioOpen(agi::AudioProvider *provider) {
	try {
		impl->content_worker.SetProvider(provider);
		Refresh(false);
	}
	catch (std::exception const& err) {
		RequestFallback(std::string("Skia Audio content worker initialization failed: ") + err.what());
	}
	catch (...) {
		RequestFallback("an unknown exception escaped Skia Audio content worker initialization");
	}
}

void SkiaAudioDisplay::RequestFallback(std::string message) {
	if (impl->fallback_requested)
		return;
	impl->fallback_requested = true;
	if (message.empty())
		message = "Skia Audio Display failed without a device diagnostic";
	if (impl->failure_callback)
		impl->failure_callback(std::move(message));
}

void SkiaAudioDisplay::ClearFailureCallback() {
	impl->failure_callback = {};
}

void SkiaAudioDisplay::SyncToCurrentAudioProvider() {
	OnAudioOpen(impl->project_context->GetCore().project->AudioProvider());
}

}
