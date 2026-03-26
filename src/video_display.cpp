// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file video_display.cpp
/// @brief Control displaying a video frame obtained from the video context
/// @ingroup video main_ui
///

#include "video_display.h"

#include "ass_file.h"
#include "async_video_provider.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "retina_helper.h"
#include "spline_curve.h"
#include "utils.h"
#include "video_render_opengl_proc_loader.h"
#include "video_renderer_factory.h"
#include "video_renderer_error.h"
#include "video_renderer_opengl.h"
#include "video_render_routing.h"
#include "video_display_layout.h"
#include "video_memory_stats.h"
#include "video_zoom.h"
#include "video_controller.h"
#include "video_frame.h"
#include "visual_tool.h"

#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <wx/combobox.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/toolbar.h>

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

/// Attribute list for gl canvases; set the canvases to doublebuffered rgba with an 8 bit stencil buffer
#if wxCHECK_VERSION (3, 1, 1)
// Explicitly set buffer to 24-bit color + 8-bit alpha. See https://github.com/wangqr/Aegisub/issues/55
int attribList[] = { WX_GL_RGBA , WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, WX_GL_BUFFER_SIZE, 24, WX_GL_MIN_ALPHA, 8, 0 };
#else
int attribList[] = { WX_GL_RGBA , WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

/// An OpenGL error occurred while uploading or displaying a frame
class OpenGlException final : public agi::Exception {
public:
	OpenGlException(const char *func, int err)
	: agi::Exception(agi::format("%s failed with error code %d", func, err))
	{ }
};

#define E(cmd) cmd; if (GLenum err = glGetError()) throw OpenGlException(#cmd, err)

namespace {
template <typename Proc>
Proc LoadOptionalProc(char const *name, char const *fallback_name = nullptr) {
	if (auto *proc = opengl::GetProcAddress(name))
		return reinterpret_cast<Proc>(proc);
	if (fallback_name) {
		if (auto *proc = opengl::GetProcAddress(fallback_name))
			return reinterpret_cast<Proc>(proc);
	}
	return nullptr;
}

struct CaptureFramebufferFunctions {
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
};

CaptureFramebufferFunctions const& GetCaptureFramebufferFunctions() {
	static const CaptureFramebufferFunctions functions = {
		LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT"),
		LoadOptionalProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT"),
		LoadOptionalProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT"),
		LoadOptionalProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"),
	};
	return functions;
}

wxImage GetBgraFallbackImage(agi::Context *context, int frame_number, double frame_time, bool raw) {
	auto frame = context->project->VideoProvider()->GetFrameBgra(frame_number, frame_time, raw);
	if (!frame || frame->data.empty())
		return {};
	return GetImage(*frame);
}


VideoMemorySnapshot BuildVideoMemorySnapshot(agi::Context *context, VideoDisplay const* display) {
	VideoMemorySnapshot snapshot;
	if (!context || !context->project)
		return snapshot;

	if (auto* provider = context->project->VideoProvider())
		snapshot.async = provider->CollectMemoryStats();
	if (auto* audio_provider = context->project->AudioProvider())
		snapshot.audio = audio_provider->GetMemoryStats();
	if (display)
		snapshot.display = display->CollectMemoryStats();
	return snapshot;
}
}

VideoDisplay::VideoDisplay(wxToolBar *toolbar, bool freeSize, wxComboBox *zoomBox, wxWindow *parent, agi::Context *c)
: wxGLCanvas(parent, -1, attribList)
, autohideTools(OPT_GET("Tool/Visual/Autohide"))
, con(c)
, zoomValue(OPT_GET("Video/Default Zoom")->GetInt() * .125 + .125)
, toolBar(toolbar)
, zoomBox(zoomBox)
, freeSize(freeSize)
, retina_helper(agi::make_unique<RetinaHelper>(this))
, scale_factor(retina_helper->GetScaleFactor())
, scale_factor_connection(retina_helper->AddScaleFactorListener([=](int new_scale_factor) {
	scale_factor = new_scale_factor;
	RefreshVideoScale();
}))
, dpi_scale_option_connection(OPT_SUB("Video/Scale with DPI", [=](agi::OptionValue const&) { RefreshVideoScale(); }))
, renderer_backend_option_connection(OPT_SUB("Video/Renderer/Backend", &VideoDisplay::OnRendererBackendChanged, this))
{
	zoomBox->SetValue(fmt_wx("%g%%", zoomValue * 100.));
	zoomBox->Bind(wxEVT_COMBOBOX, &VideoDisplay::SetZoomFromBox, this);
	zoomBox->Bind(wxEVT_TEXT_ENTER, &VideoDisplay::SetZoomFromBoxText, this);

	con->videoController->Bind(EVT_FRAME_READY, &VideoDisplay::UploadFrameData, this);
	connections = agi::signal::make_vector({
		con->project->AddVideoProviderListener(&VideoDisplay::OnVideoProviderChanged, this),
		con->videoController->AddARChangeListener(&VideoDisplay::UpdateSize, this),
	});

	Bind(wxEVT_PAINT, std::bind(&VideoDisplay::Render, this));
	Bind(wxEVT_IDLE, &VideoDisplay::OnIdle, this);
	Bind(wxEVT_SIZE, &VideoDisplay::OnSizeEvent, this);
	Bind(wxEVT_CONTEXT_MENU, &VideoDisplay::OnContextMenu, this);
	Bind(wxEVT_ENTER_WINDOW, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_CHAR_HOOK, &VideoDisplay::OnKeyDown, this);
	Bind(wxEVT_LEAVE_WINDOW, &VideoDisplay::OnMouseLeave, this);
	Bind(wxEVT_LEFT_DCLICK, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_UP, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOTION, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOUSEWHEEL, &VideoDisplay::OnMouseWheel, this);

	SetCursor(wxNullCursor);

	c->videoDisplay = this;

	con->videoController->JumpToFrame(con->videoController->GetFrameN());

	SetLayoutDirection(wxLayout_LeftToRight);
}

VideoDisplay::~VideoDisplay () {
	Unload();
	con->videoController->Unbind(EVT_FRAME_READY, &VideoDisplay::UploadFrameData, this);
}

double VideoDisplay::GetVideoScaleFactor() const {
	if (!OPT_GET("Video/Scale with DPI")->GetBool())
		return 1.0;
	return GetWindowScaleFactor(const_cast<VideoDisplay *>(this));
}

bool VideoDisplay::InitContext() {
	if (!IsShownOnScreen())
		return false;

	// If this display is in a minimized detached dialog IsShownOnScreen will
	// return true, but the client size is guaranteed to be 0
	if (GetClientSize() == wxSize(0, 0))
		return false;

	if (!glContext)
		glContext = agi::make_unique<wxGLContext>(this);

	SetCurrent(*glContext);
	return true;
}

void VideoDisplay::ResetRenderers() {
	if (glContext)
		SetCurrent(*glContext);

	if (videoRenderer)
		videoRenderer->Reset();
	videoRenderer.reset();

	if (subtitleOverlayRenderer)
		subtitleOverlayRenderer->Reset();
	subtitleOverlayRenderer.reset();
}

bool VideoDisplay::ApplyRendererSourceModePreference() {
	auto* provider = con->project->VideoProvider();
	if (!provider || !videoRenderer)
		return false;

	return provider->SetPreferredSourceModes(videoRenderer->GetPreferredSourceModes());
}

void VideoDisplay::OnRendererBackendChanged(agi::OptionValue const&) {
	if (!con->project->VideoProvider())
		return;

	if (!has_pending_packet && has_displayed_packet) {
		pending_packet = displayed_packet;
		has_pending_packet = true;
	}

	ResetRenderers();

	if (has_pending_packet)
		DoRender();
}

void VideoDisplay::OnVideoProviderChanged(AsyncVideoProvider *provider) {
	pending_packet = { };
	has_pending_packet = false;
	displayed_packet = { };
	has_displayed_packet = false;
	ResetRenderers();

	if (!provider)
		return;

	UpdateSize();
}

void VideoDisplay::UploadFrameData(FrameReadyEvent &evt) {
	pending_packet = std::move(evt.packet);
	has_pending_packet = true;

	// Instead of calling Render(), we force a render here to minimize delay
	DoRender();
}

VideoDisplayMemoryStats VideoDisplay::CollectMemoryStats() const {
	VideoDisplayMemoryStats stats;
	if (has_pending_packet)
		stats.pending_packet_ref_bytes = EstimateVideoRenderPacketReferencedBytes(pending_packet);
	if (has_displayed_packet)
		stats.displayed_packet_ref_bytes = EstimateVideoRenderPacketReferencedBytes(displayed_packet);
	if (videoRenderer) {
		stats.primary_renderer_name = videoRenderer->GetDebugName();
		stats.primary_renderer_texture_bytes = videoRenderer->EstimateTextureBytes();
	}
	if (subtitleOverlayRenderer) {
		stats.secondary_renderer_name = subtitleOverlayRenderer->GetDebugName();
		stats.secondary_renderer_texture_bytes = subtitleOverlayRenderer->EstimateTextureBytes();
	}
	return stats;
}

void VideoDisplay::Render() {
	render_requested = true;
}

wxImage VideoDisplay::CapturePacketImage(VideoRenderPacket const& packet) {
	auto* provider = con->project->VideoProvider();
	if (!provider || !packet.source_frame.IsValid())
		return {};

	int const width = provider->GetWidth();
	int const height = provider->GetHeight();
	if (width <= 0 || height <= 0)
		return {};

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		return {};
	}

	GLint previous_framebuffer = 0;
	E(glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &previous_framebuffer));

	GLuint framebuffer = 0;
	GLuint texture = 0;
	auto cleanup = agi::make_scope_exit([&] {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(previous_framebuffer));
		if (texture)
			glDeleteTextures(1, &texture);
		if (framebuffer)
			gl.DeleteFramebuffers(1, &framebuffer);
	});

	gl.GenFramebuffers(1, &framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glGenFramebuffers", err);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glGenTextures(1, &texture));
	E(glBindTexture(GL_TEXTURE_2D, texture));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	E(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
	if (GLenum err = glGetError())
		throw OpenGlException("glFramebufferTexture2D", err);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		throw agi::InternalError("Failed to create an offscreen framebuffer for video capture.");

	auto renderer_result = CreateConfiguredVideoRenderer();
	auto capture_renderer = std::move(renderer_result.renderer);
	std::unique_ptr<IVideoRenderer> capture_overlay_renderer;

	auto const routing = DecideVideoRenderRouting(packet, capture_renderer->SupportsDirectOverlay());
	if (routing == VideoRenderRoutingMode::SourceFrameOnly) {
		capture_renderer->UploadFrame(packet.source_frame);
		capture_renderer->UploadOverlay(nullptr);
	}
	else if (routing == VideoRenderRoutingMode::PrimaryRendererDirectOverlay) {
		capture_renderer->UploadFrame(packet.source_frame);
		capture_renderer->UploadOverlay(&packet.subtitle_overlay);
	}
	else if (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay) {
		capture_renderer->UploadFrame(packet.source_frame);
		capture_renderer->UploadOverlay(nullptr);
		capture_overlay_renderer = agi::make_unique<OpenGLVideoRenderer>(false, true, false);
		capture_overlay_renderer->UploadFrame(packet.source_frame);
		capture_overlay_renderer->UploadOverlay(&packet.subtitle_overlay);
	}
	else {
		auto display_frame = packet.DisplayFrame();
		if (!display_frame || display_frame->data.empty())
			throw agi::InternalError("Video capture needs a composited BGRA frame for fallback routing.");
		capture_renderer->UploadFrame(MakeBakedSourceFrameView(*display_frame, packet.source_frame));
		capture_renderer->UploadOverlay(nullptr);
	}

	capture_renderer->Render({ 0, 0, width, height }, width, height);
	if (capture_overlay_renderer)
		capture_overlay_renderer->Render({ 0, 0, width, height }, width, height);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glViewport(0, 0, width, height));
	E(glFlush());

	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = static_cast<size_t>(width) * 4;
	frame.flipped = true;
	frame.data.resize(frame.pitch * frame.height);
	E(glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, frame.data.data()));
	return GetImage(frame);
}

wxImage VideoDisplay::CaptureCurrentRenderersImage() {
	auto* provider = con->project->VideoProvider();
	if (!provider || !videoRenderer)
		return {};

	int const width = provider->GetWidth();
	int const height = provider->GetHeight();
	if (width <= 0 || height <= 0)
		return {};

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		return {};
	}

	GLint previous_framebuffer = 0;
	E(glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &previous_framebuffer));

	GLuint framebuffer = 0;
	GLuint texture = 0;
	auto cleanup = agi::make_scope_exit([&] {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(previous_framebuffer));
		if (texture)
			glDeleteTextures(1, &texture);
		if (framebuffer)
			gl.DeleteFramebuffers(1, &framebuffer);
	});

	gl.GenFramebuffers(1, &framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glGenFramebuffers", err);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glGenTextures(1, &texture));
	E(glBindTexture(GL_TEXTURE_2D, texture));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	E(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
	if (GLenum err = glGetError())
		throw OpenGlException("glFramebufferTexture2D", err);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		throw agi::InternalError("Failed to create an offscreen framebuffer for video capture.");

	videoRenderer->Render({ 0, 0, width, height }, width, height);
	if (subtitleOverlayRenderer)
		subtitleOverlayRenderer->Render({ 0, 0, width, height }, width, height);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glViewport(0, 0, width, height));
	E(glFlush());

	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = static_cast<size_t>(width) * 4;
	frame.flipped = true;
	frame.data.resize(frame.pitch * frame.height);
	E(glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, frame.data.data()));
	return GetImage(frame);
}

wxImage VideoDisplay::GetFrameImage(bool raw) {
	auto* provider = con->project->VideoProvider();
	if (!provider)
		return {};

	int const frame_number = con->videoController->GetFrameN();
	double const frame_time = con->project->Timecodes().TimeAtFrame(frame_number);
	if (!InitContext())
		return GetBgraFallbackImage(con, frame_number, frame_time, raw);

	VideoRenderPacket packet;
	if (!raw && has_displayed_packet) {
		packet = displayed_packet;
	}
	else {
		packet = provider->GetRenderPacket(frame_number, frame_time, raw);
	}

	try {
		auto image = CapturePacketImage(packet);
		if (image.IsOk())
			return image;
	}
	catch (agi::Exception const&) {
	}

	return GetBgraFallbackImage(con, frame_number, frame_time, raw);
}

void VideoDisplay::OnIdle(wxIdleEvent&) {
	if (render_requested)
		DoRender();
}

void VideoDisplay::DoRender() try {
	render_requested = false;

	if (!con->project->VideoProvider() || !InitContext() || (!videoRenderer && !has_pending_packet))
		return;

	if (!videoRenderer) {
		auto renderer_result = CreateConfiguredVideoRenderer();
		videoRenderer = std::move(renderer_result.renderer);
		if (ApplyRendererSourceModePreference()) {
			pending_packet = { };
			has_pending_packet = false;
			displayed_packet = { };
			has_displayed_packet = false;
			con->videoController->JumpToFrame(con->videoController->GetFrameN());
			return;
		}
	}

	if (!tool)
		cmd::call("video/tool/cross", con);

	try {
		if (has_pending_packet) {
			bool const first_presented_frame = !has_displayed_packet;
			auto const routing = DecideVideoRenderRouting(
				pending_packet,
				videoRenderer->SupportsDirectOverlay());

			if (routing == VideoRenderRoutingMode::SourceFrameOnly) {
				videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(nullptr);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			else if (routing == VideoRenderRoutingMode::PrimaryRendererDirectOverlay) {
				videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(&pending_packet.subtitle_overlay);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			else if (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay) {
				videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(nullptr);
				if (!subtitleOverlayRenderer)
					subtitleOverlayRenderer = agi::make_unique<OpenGLVideoRenderer>(false, true, false);
				subtitleOverlayRenderer->UploadFrame(pending_packet.source_frame);
				subtitleOverlayRenderer->UploadOverlay(&pending_packet.subtitle_overlay);
			}
			else {
				auto display_frame = pending_packet.DisplayFrame();
				videoRenderer->UploadFrame(MakeBakedSourceFrameView(*display_frame, pending_packet.source_frame));
				videoRenderer->UploadOverlay(nullptr);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			displayed_packet = pending_packet;
			has_displayed_packet = true;
			pending_packet = { };
			has_pending_packet = false;
			FramePresented(displayed_packet.frame_number);
			auto ui = con->GetUI();
			ui.videoFramePresented(displayed_packet.frame_number);
			if (perf_trace::ShouldSampleVideoMemory(first_presented_frame)) {
				auto snapshot = BuildVideoMemorySnapshot(con, this);
				perf_trace::ObserveVideoMemorySnapshot("frame_presented", snapshot, first_presented_frame);
			}
		}
	}
	catch (const VideoOutInitException& err) {
		wxLogError(
			wxS("Failed to initialize video display. Closing other running "
			    "programs and updating your video card drivers may fix this.\n"
			    "Error message reported: %s"),
			to_wx(err.GetMessage()));
		con->project->CloseVideo();
		return;
	}
	catch (const VideoOutRenderException& err) {
		wxLogError(
			wxS("Could not upload video frame to graphics card.\n"
			    "Error message reported: %s"),
			to_wx(err.GetMessage()));
		return;
	}

	if (videoSize.GetWidth() == 0) videoSize.SetWidth(1);
	if (videoSize.GetHeight() == 0) videoSize.SetHeight(1);

	if (!viewport_height || !viewport_width)
		PositionVideo();

	videoRenderer->Render({ viewport_left, viewport_bottom, viewport_width, viewport_height }, GetClientSize().GetWidth() * scale_factor, GetClientSize().GetHeight() * scale_factor);
	if (subtitleOverlayRenderer)
		subtitleOverlayRenderer->Render({ viewport_left, viewport_bottom, viewport_width, viewport_height }, GetClientSize().GetWidth() * scale_factor, GetClientSize().GetHeight() * scale_factor);
	E(glViewport(0, std::min(viewport_bottom, 0), videoSize.GetWidth(), videoSize.GetHeight()));

	E(glMatrixMode(GL_PROJECTION));
	E(glLoadIdentity());
	E(glOrtho(0.0f, videoSize.GetWidth() / scale_factor, videoSize.GetHeight() / scale_factor, 0.0f, -1000.0f, 1000.0f));

	if (OPT_GET("Video/Overscan Mask")->GetBool()) {
		double ar = con->videoController->GetAspectRatioValue();

		// Based on BBC's guidelines: http://www.bbc.co.uk/guidelines/dq/pdf/tv/tv_standards_london.pdf
		// 16:9 or wider
		if (ar > 1.75) {
			DrawOverscanMask(.1f, .05f);
			DrawOverscanMask(0.035f, 0.035f);
		}
		// Less wide than 16:9 (use 4:3 standard)
		else {
			DrawOverscanMask(.067f, .05f);
			DrawOverscanMask(0.033f, 0.035f);
		}
	}

	if ((mouse_pos || !autohideTools->GetBool()) && tool)
		tool->Draw();

	SwapBuffers();
}
catch (const agi::Exception &err) {
	wxLogError(
		wxS("An error occurred trying to render the video frame on the screen.\n"
		    "Error message reported: %s"),
		to_wx(err.GetMessage()));
	con->project->CloseVideo();
}

void VideoDisplay::DrawOverscanMask(float horizontal_percent, float vertical_percent) const {
	Vector2D v(viewport_width, viewport_height);
	Vector2D size = Vector2D(horizontal_percent, vertical_percent) / 2 * v;

	// Clockwise from top-left
	Vector2D corners[] = {
		size,
		Vector2D(viewport_width - size.X(), size),
		v - size,
		Vector2D(size, viewport_height - size.Y())
	};

	// Shift to compensate for black bars
	Vector2D pos(viewport_left, viewport_top);
	for (auto& corner : corners)
		corner = corner + pos;

	int count = 0;
	std::vector<float> points;
	for (size_t i = 0; i < 4; ++i) {
		size_t prev = (i + 3) % 4;
		size_t next = (i + 1) % 4;
		count += SplineCurve(
				(corners[prev] + corners[i] * 4) / 5,
				corners[i], corners[i],
				(corners[next] + corners[i] * 4) / 5)
			.GetPoints(points);
	}

	OpenGLWrapper gl;
	gl.SetFillColour(wxColor(30, 70, 200), .5f);
	gl.SetLineColour(*wxBLACK, 0, 1);

	std::vector<int> vstart(1, 0);
	std::vector<int> vcount(1, count);
	gl.DrawMultiPolygon(points, vstart, vcount, Vector2D(viewport_left, viewport_top), Vector2D(viewport_width, viewport_height), true);
}

void VideoDisplay::PositionVideo() {
	auto provider = con->project->VideoProvider();
	if (!provider || !IsShownOnScreen()) return;

	AspectRatio arType = con->videoController->GetAspectRatioType();
	double target_aspect_ratio = 0.0;
	if (freeSize) {
		target_aspect_ratio = arType == AspectRatio::Default
			? static_cast<double>(provider->GetWidth()) / provider->GetHeight()
			: con->videoController->GetAspectRatioValue();
	}
	auto layout = BuildVideoDisplayViewportLayout(
		GetClientSize().GetWidth() * scale_factor,
		GetClientSize().GetHeight() * scale_factor,
		videoSize.GetWidth(),
		videoSize.GetHeight(),
		freeSize,
		target_aspect_ratio);
	viewport_left = layout.viewport_left;
	viewport_width = layout.viewport_width;
	viewport_bottom = layout.viewport_bottom;
	viewport_top = layout.viewport_top;
	viewport_height = layout.viewport_height;

	if (tool)
		tool->SetDisplayArea(viewport_left / scale_factor, viewport_top / scale_factor,
			viewport_width / scale_factor, viewport_height / scale_factor);

	Render();
}

void VideoDisplay::UpdateSize() {
	auto provider = con->project->VideoProvider();
	if (!provider || !IsShownOnScreen()) return;

	videoSize.Set(provider->GetWidth(), provider->GetHeight());
	videoSize *= zoomValue * GetVideoScaleFactor();
	if (con->videoController->GetAspectRatioType() != AspectRatio::Default)
		videoSize.SetWidth(videoSize.GetHeight() * con->videoController->GetAspectRatioValue());

	wxEventBlocker blocker(this);
	if (freeSize) {
		wxWindow *top = GetParent();
		while (!top->IsTopLevel()) top = top->GetParent();

		wxSize cs = GetClientSize();
		wxSize oldSize = top->GetSize();
		top->SetSize(top->GetSize() + videoSize / scale_factor - cs);
		SetClientSize(cs + top->GetSize() - oldSize);
	}
	else {
		SetMinClientSize(videoSize / scale_factor);
		SetMaxClientSize(videoSize / scale_factor);

		GetParent()->Layout();
		GetGrandParent()->Layout();
	}

	PositionVideo();
}

void VideoDisplay::RefreshVideoScale() {
	if (tool && toolBar) {
		toolBar->ClearTools();
		tool->SetToolbar(toolBar);
		GetParent()->Layout();
		GetGrandParent()->Layout();
	}
	if (con->project->VideoProvider())
		UpdateSize();
}

void VideoDisplay::OnSizeEvent(wxSizeEvent &event) {
	if (freeSize) {
		videoSize = GetClientSize() * scale_factor;
		PositionVideo();
		zoomValue = double(viewport_height) / con->project->VideoProvider()->GetHeight();
		zoomBox->ChangeValue(fmt_wx("%g%%", zoomValue * 100.));
		con->ass->Properties.video_zoom = zoomValue;
	}
	else {
		PositionVideo();
	}
}

void VideoDisplay::OnMouseEvent(wxMouseEvent& event) {
	if (event.ButtonDown())
		SetFocus();

	last_mouse_pos = mouse_pos = event.GetPosition();

	if (tool)
		tool->OnMouseEvent(event);
}

void VideoDisplay::OnMouseLeave(wxMouseEvent& event) {
	mouse_pos = Vector2D();
	if (tool)
		tool->OnMouseEvent(event);
}

void VideoDisplay::OnMouseWheel(wxMouseEvent& event) {
	if (int wheel = event.GetWheelRotation()) {
		if (ForwardMouseWheelEvent(this, event)) {
			int wheel_steps = wheel / event.GetWheelDelta();
			if (freeSize)
				SetZoom(AdvanceDetachedVideoZoomByWheel(zoomValue, wheel_steps, zoomBox->GetCount()));
			else
				SetZoom(zoomValue + kVideoZoomStep * wheel_steps);
		}
	}
}

void VideoDisplay::OnContextMenu(wxContextMenuEvent&) {
	if (!context_menu) context_menu = menu::GetMenu("video_context", (wxID_HIGHEST + 1) + 9000, con);
	SetCursor(wxNullCursor);
	menu::OpenPopupMenu(context_menu.get(), this);
}

void VideoDisplay::OnKeyDown(wxKeyEvent &event) {
	hotkey::check("Video", con, event);
}

void VideoDisplay::SetZoom(double value) {
	if (value == 0) return;
	zoomValue = std::max(value, .125);
	size_t selIndex = zoomValue / .125 - 1;
	if (selIndex < zoomBox->GetCount())
		zoomBox->SetSelection(selIndex);
	zoomBox->ChangeValue(fmt_wx("%g%%", zoomValue * 100.));
	con->ass->Properties.video_zoom = zoomValue;
	UpdateSize();
}

void VideoDisplay::SetZoomFromBox(wxCommandEvent &) {
	int sel = zoomBox->GetSelection();
	if (sel != wxNOT_FOUND) {
		zoomValue = (sel + 1) * .125;
		con->ass->Properties.video_zoom = zoomValue;
		UpdateSize();
	}
}

void VideoDisplay::SetZoomFromBoxText(wxCommandEvent &) {
	wxString strValue = zoomBox->GetValue();
	if (strValue.EndsWith(wxS("%")))
		strValue.RemoveLast();

	double value;
	if (strValue.ToDouble(&value))
		SetZoom(value / 100.);
}

void VideoDisplay::SetTool(std::unique_ptr<VisualToolBase> new_tool) {
	// Set the tool first to prevent repeated initialization from VideoDisplay::Render
	tool = std::move(new_tool);

	// Hide the tool bar first to eliminate unecessary size changes
	toolBar->Show(false);
	toolBar->ClearTools();
	tool->SetToolbar(toolBar);

	// Update size as the new typesetting tool may have changed the subtoolbar size
	if (!freeSize)
		UpdateSize();
	else {
		// UpdateSize fits the window to the video, which we don't want to do
		GetParent()->Layout();
		GetGrandParent()->Layout();
		tool->SetDisplayArea(viewport_left / scale_factor, viewport_top / scale_factor,
			viewport_width / scale_factor, viewport_height / scale_factor);
	}
}

bool VideoDisplay::ToolIsType(std::type_info const& type) const {
	return tool && typeid(*tool) == type;
}

Vector2D VideoDisplay::GetMousePosition() const {
	return last_mouse_pos ? tool->ToScriptCoords(last_mouse_pos) : last_mouse_pos;
}

void VideoDisplay::Unload() {
	ResetRenderers();
	tool.reset();
	glContext.reset();
	pending_packet = { };
	has_pending_packet = false;
	displayed_packet = { };
	has_displayed_packet = false;
}
