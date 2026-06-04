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
#include "ass_time_projection.h"
#include "async_video_provider.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"
#include "legacy_gl_draw.h"
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
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <wx/combobox.h>
#include <wx/dcclient.h>
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

#ifdef WITH_SKIA
#include "skia_runtime/skia_gpu_context_host.h"
#include "skia_runtime/skia_surface_provider.h"
#include "skia_runtime/skia_text_layout_cache.h"
#include "video_overlay_draw_context_skia.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
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
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = nullptr;
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = nullptr;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = nullptr;
	PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage = nullptr;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = nullptr;
};

struct ScopedFramebufferState {
	CaptureFramebufferFunctions const& gl;
	GLint framebuffer = 0;
	GLint draw_buffer = GL_BACK;
	GLint read_buffer = GL_BACK;
	GLint viewport[4] = { 0, 0, 0, 0 };

	explicit ScopedFramebufferState(CaptureFramebufferFunctions const& gl)
	: gl(gl) {
		glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
		glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
	}

	~ScopedFramebufferState() {
		if (gl.BindFramebuffer)
			gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(framebuffer));
		glDrawBuffer(static_cast<GLenum>(draw_buffer));
		glReadBuffer(framebuffer == 0 ? GL_BACK : static_cast<GLenum>(read_buffer));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		legacy_gl::ResetCompatibilityState();
	}
};

CaptureFramebufferFunctions const& GetCaptureFramebufferFunctions() {
	static const CaptureFramebufferFunctions functions = {
		LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT"),
		LoadOptionalProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT"),
		LoadOptionalProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT"),
		LoadOptionalProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"),
		LoadOptionalProc<PFNGLBINDRENDERBUFFERPROC>("glBindRenderbuffer", "glBindRenderbufferEXT"),
		LoadOptionalProc<PFNGLDELETERENDERBUFFERSPROC>("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT"),
		LoadOptionalProc<PFNGLGENRENDERBUFFERSPROC>("glGenRenderbuffers", "glGenRenderbuffersEXT"),
		LoadOptionalProc<PFNGLRENDERBUFFERSTORAGEPROC>("glRenderbufferStorage", "glRenderbufferStorageEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERRENDERBUFFERPROC>("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT"),
	};
	return functions;
}

void BindWindowFramebufferForDisplayRender() {
	auto const& gl = GetCaptureFramebufferFunctions();
	if (gl.BindFramebuffer) {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
		if (GLenum err = glGetError())
			throw OpenGlException("glBindFramebuffer", err);
		E(glDrawBuffer(GL_BACK));
		E(glReadBuffer(GL_BACK));
	}
	legacy_gl::ResetCompatibilityState();
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

void ApplyViewportLayout(
	VideoDisplayViewportLayout const& layout,
	int& viewport_left,
	int& viewport_width,
	int& viewport_bottom,
	int& viewport_top,
	int& viewport_height) {
	viewport_left = layout.viewport_left;
	viewport_width = layout.viewport_width;
	viewport_bottom = layout.viewport_bottom;
	viewport_top = layout.viewport_top;
	viewport_height = layout.viewport_height;
}

bool SourceFrameColorMetadataEquals(
	SourceFrameColorMetadata const& lhs,
	SourceFrameColorMetadata const& rhs) {
	return lhs.matrix == rhs.matrix
		&& lhs.primaries == rhs.primaries
		&& lhs.transfer == rhs.transfer
		&& lhs.range == rhs.range;
}

bool SourceFrameGeometryEquals(
	SourceFrameGeometry const& lhs,
	SourceFrameGeometry const& rhs) {
	return lhs.storage_width == rhs.storage_width
		&& lhs.storage_height == rhs.storage_height
		&& lhs.visible_rect.x == rhs.visible_rect.x
		&& lhs.visible_rect.y == rhs.visible_rect.y
		&& lhs.visible_rect.width == rhs.visible_rect.width
		&& lhs.visible_rect.height == rhs.visible_rect.height
		&& lhs.rotation == rhs.rotation
		&& lhs.display_vflip == rhs.display_vflip
		&& lhs.pixel_aspect_ratio == rhs.pixel_aspect_ratio;
}

bool SourceFrameNativeFormatIdentityEquals(
	SourceFrameNativeFormatIdentity const& lhs,
	SourceFrameNativeFormatIdentity const& rhs) {
	return lhs.format_namespace == rhs.format_namespace
		&& lhs.format_id == rhs.format_id;
}

bool SourceFrameFloatEquals(float lhs, float rhs) {
	return std::fabs(lhs - rhs) <= 0.000001f;
}

inline bool SourceFrameValueEquals(float lhs, float rhs) {
	return SourceFrameFloatEquals(lhs, rhs);
}

template <typename T>
bool SourceFrameValueEquals(T const& lhs, T const& rhs);

template <typename T, size_t N>
bool SourceFrameValueEquals(std::array<T, N> const& lhs, std::array<T, N> const& rhs);

template <typename T, size_t N>
bool SourceFrameArrayEquals(std::array<T, N> const& lhs, std::array<T, N> const& rhs) {
	for (size_t i = 0; i < N; ++i) {
		if (!SourceFrameValueEquals(lhs[i], rhs[i]))
			return false;
	}
	return true;
}

template <typename T, size_t N>
bool SourceFrameValueEquals(std::array<T, N> const& lhs, std::array<T, N> const& rhs) {
	return SourceFrameArrayEquals(lhs, rhs);
}

template <typename T>
bool SourceFrameValueEquals(T const& lhs, T const& rhs) {
	return lhs == rhs;
}

bool SourceFrameDolbyVisionComponentEquals(
	SourceFrameDolbyVisionReshapeComponent const& lhs,
	SourceFrameDolbyVisionReshapeComponent const& rhs) {
	return lhs.num_pivots == rhs.num_pivots
		&& SourceFrameArrayEquals(lhs.pivots, rhs.pivots)
		&& SourceFrameArrayEquals(lhs.method, rhs.method)
		&& SourceFrameArrayEquals(lhs.poly_coeffs, rhs.poly_coeffs)
		&& SourceFrameArrayEquals(lhs.mmr_order, rhs.mmr_order)
		&& SourceFrameArrayEquals(lhs.mmr_constant, rhs.mmr_constant)
		&& SourceFrameArrayEquals(lhs.mmr_coeffs, rhs.mmr_coeffs);
}

bool SourceFrameDolbyVisionMetadataEquals(
	SourceFrameDolbyVisionMetadata const& lhs,
	SourceFrameDolbyVisionMetadata const& rhs) {
	if (lhs.valid != rhs.valid)
		return false;
	if (!lhs.valid)
		return true;
	if (lhs.bl_bit_depth != rhs.bl_bit_depth
		|| lhs.coefficient_log2_denom != rhs.coefficient_log2_denom
		|| lhs.has_l1 != rhs.has_l1
		|| !SourceFrameArrayEquals(lhs.nonlinear_offset, rhs.nonlinear_offset)
		|| !SourceFrameArrayEquals(lhs.nonlinear, rhs.nonlinear)
		|| !SourceFrameArrayEquals(lhs.linear, rhs.linear)
		|| !SourceFrameFloatEquals(lhs.source_min_pq, rhs.source_min_pq)
		|| !SourceFrameFloatEquals(lhs.source_max_pq, rhs.source_max_pq)
		|| !SourceFrameFloatEquals(lhs.max_pq_y, rhs.max_pq_y)
		|| !SourceFrameFloatEquals(lhs.avg_pq_y, rhs.avg_pq_y)
		|| lhs.rpu != rhs.rpu)
		return false;
	for (size_t i = 0; i < lhs.comp.size(); ++i) {
		if (!SourceFrameDolbyVisionComponentEquals(lhs.comp[i], rhs.comp[i]))
			return false;
	}
	return true;
}

bool SourceFrameEquivalentForUpload(
	SourceFrame const& lhs,
	SourceFrame const& rhs) {
	return lhs.output_mode == rhs.output_mode
		&& lhs.pixel_format == rhs.pixel_format
		&& SourceFrameNativeFormatIdentityEquals(lhs.native_format, rhs.native_format)
		&& SourceFrameFormatInfoEquals(lhs.format_info, rhs.format_info)
		&& lhs.width == rhs.width
		&& lhs.height == rhs.height
		&& lhs.flipped == rhs.flipped
		&& lhs.plane_count == rhs.plane_count
		&& SourceFrameColorMetadataEquals(lhs.color, rhs.color)
		&& SourceFrameDolbyVisionMetadataEquals(lhs.dolby_vision, rhs.dolby_vision)
		&& lhs.chroma_location == rhs.chroma_location
		&& SourceFrameGeometryEquals(lhs.geometry, rhs.geometry);
}

bool ReadEnvFlagDefaultOn(char const *name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return true;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

bool IsSkiaVideoOverlayEnabled() {
#ifdef WITH_SKIA
	return ReadEnvFlagDefaultOn("AEGISUB_ENABLE_SKIA_VIDEO_OVERLAY");
#else
	return false;
#endif
}

}

VideoDisplay::VideoDisplay(wxToolBar *toolbar, bool freeSize, wxComboBox *zoomBox, wxWindow *parent, agi::Context *c)
: wxGLCanvas(parent, -1, attribList)
, autohideTools(OPT_GET("Tool/Visual/Autohide"))
, scrollAction(OPT_GET("Video/Scroll Action"))
, ctrlScrollAction(OPT_GET("Video/Ctrl Scroll Action"))
, shiftScrollAction(OPT_GET("Video/Shift Scroll Action"))
, con(c)
, zoomValue(OPT_GET("Video/Default Zoom")->GetInt() * .125 + .125)
, toolBar(toolbar)
, zoomBox(zoomBox)
, freeSize(freeSize)
, retina_helper(agi::make_unique<RetinaHelper>(this))
, scale_factor(retina_helper->GetScaleFactor())
, scene_cache_enabled(ReadEnvFlagDefaultOn("AEGISUB_ENABLE_VIDEO_SCENE_CACHE"))
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

	connections = agi::signal::make_vector({
		con->videoController->AddFrameReadyListener(&VideoDisplay::UploadFrameData, this),
		con->project->AddVideoProviderListener(&VideoDisplay::OnVideoProviderChanged, this),
		con->videoController->AddARChangeListener(&VideoDisplay::UpdateSize, this),
		con->ass->AddCommitListener(&VideoDisplay::OnSubtitlesCommit, this),
	});

	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Bind(wxEVT_PAINT, &VideoDisplay::OnPaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, &VideoDisplay::OnEraseBackground, this);
	Bind(wxEVT_IDLE, &VideoDisplay::OnIdle, this);
	Bind(wxEVT_SIZE, &VideoDisplay::OnSizeEvent, this);
	Bind(wxEVT_CONTEXT_MENU, &VideoDisplay::OnContextMenu, this);
	Bind(wxEVT_ENTER_WINDOW, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_CHAR_HOOK, &VideoDisplay::OnKeyDown, this);
	Bind(wxEVT_LEAVE_WINDOW, &VideoDisplay::OnMouseLeave, this);
	Bind(wxEVT_LEFT_DCLICK, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_UP, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_UP, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOTION, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOUSEWHEEL, &VideoDisplay::OnMouseWheel, this);

	SetCursor(wxNullCursor);

	c->GetUI().videoDisplay = this;

	con->videoController->JumpToFrame(con->videoController->GetFrameN());

	SetLayoutDirection(wxLayout_LeftToRight);
}

VideoDisplay::~VideoDisplay () {
	Unload();
}

void VideoDisplay::SyncToCurrentVideoProvider() {
	ApplyVideoProvider(con->project->VideoProvider());
	if (con->project->VideoProvider())
		con->videoController->JumpToFrame(con->videoController->GetFrameN());
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

void VideoDisplay::InvalidateSceneCache() {
	scene_cache_dirty = true;
	scene_cache_valid = false;
}

bool VideoDisplay::IsSceneCacheUsableForCurrentPlayback() const noexcept {
	return !con->videoController->IsPlaying();
}

bool VideoDisplay::ShouldUseSceneCacheForCurrentFrame() const noexcept {
	if (!videoRenderer)
		return false;
	if (videoRenderer->PrefersSceneCacheForRepaint())
		return true;

	// Compatibility subtitle providers bake subtitles into the uploaded BGRA
	// frame; cache that composited scene so visual tool repaints do not have
	// to redraw the full video backend on every mouse event.
	if (has_displayed_packet && !displayed_packet.allow_source_frame_upload_reuse)
		return true;
	if (has_displayed_packet
		&& DecideVideoRenderRouting(
			displayed_packet,
			videoRenderer->SupportsDirectOverlay()) == VideoRenderRoutingMode::FallbackCompositedFrame)
		return true;

	// Native source frames may require an expensive display-transform pass
	// (e.g. libplacebo YUV→RGB conversion) even when the current backend
	// does not explicitly declare a preference for scene caching.  Caching
	// the composited result avoids repeating that transform on every visual
	// tool repaint.
	return has_displayed_packet
		&& displayed_packet.source_frame.output_mode == SourceFrameOutputMode::Native;
}

bool VideoDisplay::ShouldDeferIncomingSubtitlePacket(VideoRenderPacket const& packet) const noexcept {
	if (!tool || !tool->IsInteracting())
		return false;
	if (!videoRenderer || !has_displayed_packet)
		return false;
	if (last_frame_had_separate_overlay)
		return false;
	if (!IsSceneCacheUsableForCurrentPlayback())
		return false;
	if (packet.frame_number != displayed_packet.frame_number)
		return false;

	// Compatibility providers such as CSRI have already produced a fresh baked
	// frame on the worker. Deferring it keeps the visible subtitle frozen for
	// the whole drag, so present it as soon as it arrives and let the scene
	// cache cover repaint-only mouse events between packets.
	if (!packet.allow_source_frame_upload_reuse)
		return false;

	auto const routing = DecideVideoRenderRouting(packet, videoRenderer->SupportsDirectOverlay());
	bool const packet_uses_integrated_subtitles = routing == VideoRenderRoutingMode::FallbackCompositedFrame;
	return packet_uses_integrated_subtitles && ShouldUseSceneCacheForCurrentFrame();
}

void VideoDisplay::ResetDisplayedSubtitleScene() noexcept {
	displayed_subtitle_scene.clear();
	scene_cache_waiting_for_subtitle_packet = false;
}

void VideoDisplay::ResetSceneCacheRetryBlock() noexcept {
	scene_cache_retry_blocked = false;
	scene_cache_retry_canvas_width = 0;
	scene_cache_retry_canvas_height = 0;
}

void VideoDisplay::BlockSceneCacheUntilRetry(int canvas_width, int canvas_height) noexcept {
	scene_cache_retry_blocked = true;
	scene_cache_retry_canvas_width = canvas_width;
	scene_cache_retry_canvas_height = canvas_height;
	DestroySceneCache();
}

bool VideoDisplay::ShouldAttemptSceneCache(int canvas_width, int canvas_height) noexcept {
	if (!scene_cache_enabled || canvas_width <= 0 || canvas_height <= 0)
		return false;
	if (!ShouldUseSceneCacheForCurrentFrame()) {
		if (scene_cache_framebuffer || scene_cache_texture)
			DestroySceneCache();
		return false;
	}

	if (scene_cache_retry_canvas_width != canvas_width
		|| scene_cache_retry_canvas_height != canvas_height) {
		scene_cache_retry_blocked = false;
		scene_cache_retry_canvas_width = canvas_width;
		scene_cache_retry_canvas_height = canvas_height;
	}

	return !scene_cache_retry_blocked;
}

void VideoDisplay::DestroySceneCache() noexcept {
	if (scene_cache_texture) {
		auto texture = static_cast<GLuint>(scene_cache_texture);
		glDeleteTextures(1, &texture);
		scene_cache_texture = 0;
	}
	if (scene_cache_framebuffer) {
		auto const& gl = GetCaptureFramebufferFunctions();
		if (gl.DeleteFramebuffers) {
			auto framebuffer = static_cast<GLuint>(scene_cache_framebuffer);
			gl.DeleteFramebuffers(1, &framebuffer);
		}
		scene_cache_framebuffer = 0;
	}
	scene_cache_width = 0;
	scene_cache_height = 0;
	scene_cache_valid = false;
	scene_cache_dirty = true;
}

bool VideoDisplay::EnsureSceneCache(int canvas_width, int canvas_height) {
	if (!scene_cache_enabled)
		return false;
	if (canvas_width <= 0 || canvas_height <= 0)
		return false;

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		DestroySceneCache();
		return false;
	}

	if (scene_cache_framebuffer
		&& scene_cache_texture
		&& scene_cache_width == canvas_width
		&& scene_cache_height == canvas_height) {
		legacy_gl::ResetCompatibilityState();
		return true;
	}

	DestroySceneCache();

	ScopedFramebufferState restore_state(gl);

	GLuint framebuffer = 0;
	GLuint texture = 0;
	gl.GenFramebuffers(1, &framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glGenFramebuffers", err);
	E(glGenTextures(1, &texture));

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glBindTexture(GL_TEXTURE_2D, texture));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
	E(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvas_width, canvas_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
	if (GLenum err = glGetError())
		throw OpenGlException("glFramebufferTexture2D", err);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
		glDeleteTextures(1, &texture);
		gl.DeleteFramebuffers(1, &framebuffer);
		return false;
	}
	E(glBindTexture(GL_TEXTURE_2D, 0));

	scene_cache_framebuffer = framebuffer;
	scene_cache_texture = texture;
	scene_cache_width = canvas_width;
	scene_cache_height = canvas_height;
	scene_cache_valid = false;
	scene_cache_dirty = true;
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
#ifdef WITH_SKIA
	if (skia_overlay_context_host)
		skia_overlay_context_host->Reset();
#endif
	ResetDisplayedSubtitleScene();
	ResetSceneCacheRetryBlock();
	InvalidateSceneCache();
}

bool VideoDisplay::ApplyRendererSourceModePreference() {
	auto* provider = con->project->VideoProvider();
	if (!provider || !videoRenderer)
		return false;

	bool const changed = provider->SetPreferredSourceModes(videoRenderer->GetPreferredSourceModes());
	if (changed)
		con->videoController->InvalidateRenderPacketCache();
	return changed;
}

void VideoDisplay::OnRendererBackendChanged(agi::OptionValue const&) {
	if (!con->project->VideoProvider())
		return;

	if (!has_pending_packet && has_displayed_packet) {
		pending_packet = displayed_packet;
		has_pending_packet = true;
		pending_packet_deferred_for_visual_interaction = false;
	}

	ResetRenderers();
	InvalidateSceneCache();

	if (has_pending_packet)
		DoRender();
}

void VideoDisplay::ApplyVideoProvider(AsyncVideoProvider *provider) {
	pending_packet = { };
	has_pending_packet = false;
	pending_packet_deferred_for_visual_interaction = false;
	displayed_packet = { };
	has_displayed_packet = false;
	ResetDisplayedSubtitleScene();
	contentZoomValue = 1.0;
	pan_x = 0.0;
	pan_y = 0.0;
	ResetRenderers();
	if (glContext)
		SetCurrent(*glContext);
	DestroySceneCache();

	if (!provider)
		return;

	UpdateSize();
}

void VideoDisplay::OnVideoProviderChanged(AsyncVideoProvider *provider) {
	ApplyVideoProvider(provider);
}

void VideoDisplay::UploadFrameData(VideoRenderPacket const& packet, double) {
	bool const defer_interactive_subtitle_packet = ShouldDeferIncomingSubtitlePacket(packet);
	if (defer_interactive_subtitle_packet) {
		pending_packet = packet;
		has_pending_packet = true;
		pending_packet_deferred_for_visual_interaction = true;
		scene_cache_waiting_for_subtitle_packet = false;
		render_requested = true;
		ScheduleRender();
		return;
	}

	bool const defer_interactive_playback_same_frame_packet =
		con->videoController->IsPlaying()
		&& tool
		&& tool->IsInteracting()
		&& has_displayed_packet
		&& packet.frame_number == displayed_packet.frame_number;

	bool const can_reuse_video_only_scene_cache = scene_cache_valid
		&& !scene_cache_dirty
		&& has_displayed_packet
		&& videoRenderer
		&& last_frame_had_separate_overlay
		&& DecideVideoRenderRouting(packet, videoRenderer->SupportsDirectOverlay()) == VideoRenderRoutingMode::SecondaryRendererDirectOverlay
		&& packet.allow_source_frame_upload_reuse
		&& displayed_packet.allow_source_frame_upload_reuse
		&& packet.frame_number == displayed_packet.frame_number
		&& SourceFrameEquivalentForUpload(packet.source_frame, displayed_packet.source_frame);

	pending_packet = packet;
	has_pending_packet = true;
	pending_packet_deferred_for_visual_interaction = defer_interactive_playback_same_frame_packet;
	scene_cache_waiting_for_subtitle_packet = false;
	if (!can_reuse_video_only_scene_cache || !IsSceneCacheUsableForCurrentPlayback())
		InvalidateSceneCache();
	if (defer_interactive_playback_same_frame_packet)
		return;

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
	if (scene_cache_texture && scene_cache_width > 0 && scene_cache_height > 0)
		stats.scene_cache_texture_bytes = static_cast<size_t>(scene_cache_width) * static_cast<size_t>(scene_cache_height) * 4;
	return stats;
}

void VideoDisplay::Render() {
	render_requested = true;
	ScheduleRender();
}

void VideoDisplay::RenderNow() {
	// Mouse-drag visual tool updates can keep the UI too busy for idle-driven
	// redraws; render synchronously while paused so tool feedback is not gated
	// on subtitle packet delivery cadence. During playback, video packet
	// presentation owns the full redraw cadence; mouse-motion events just update
	// tool state for the next frame instead of hammering the backend between
	// packets, which is especially fragile with libplacebo on some Win10 drivers.
	if (con->videoController->IsPlaying() && tool && tool->IsInteracting())
		return;

	render_requested = true;
	if (render_in_progress || con->videoController->IsPlaying()) {
		ScheduleRender();
		return;
	}
	DoRender();
}

void VideoDisplay::OnEraseBackground(wxEraseEvent &) {
}

void VideoDisplay::OnPaint(wxPaintEvent &) {
	wxPaintDC dc(this);
	(void)dc;
	DoRender();
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

	ScopedFramebufferState restore_state(gl);

	GLuint framebuffer = 0;
	GLuint texture = 0;
	auto cleanup = agi::make_scope_exit([&] {
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

	ScopedFramebufferState restore_state(gl);

	GLuint framebuffer = 0;
	GLuint texture = 0;
	auto cleanup = agi::make_scope_exit([&] {
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

void VideoDisplay::ScheduleRender() {
	if (render_scheduled)
		return;

	render_scheduled = true;
	CallAfter([this] {
		render_scheduled = false;
		if (render_requested)
			DoRender();
	});
}

void VideoDisplay::RenderBackendScene(int canvas_width, int canvas_height) {
	videoRenderer->Render(
		{ viewport_left, viewport_bottom, viewport_width, viewport_height },
		canvas_width,
		canvas_height);
	if (subtitleOverlayRenderer) {
		subtitleOverlayRenderer->Render(
			{ viewport_left, viewport_bottom, viewport_width, viewport_height },
			canvas_width,
			canvas_height);
	}
}

bool VideoDisplay::RenderSceneToCache(wxSize const&, int canvas_width, int canvas_height) {
	if (!EnsureSceneCache(canvas_width, canvas_height))
		return false;

	auto const& gl = GetCaptureFramebufferFunctions();
	ScopedFramebufferState restore_state(gl);

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(scene_cache_framebuffer));
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	// When the subtitle overlay is rendered by a separate pass (e.g. placebo
	// backend), only cache the video layer so that subtitle-only commits can
	// reuse the cache without a full video re-render.
	if (last_frame_had_separate_overlay) {
		videoRenderer->Render(
			{ viewport_left, viewport_bottom, viewport_width, viewport_height },
			canvas_width,
			canvas_height);
	} else {
		RenderBackendScene(canvas_width, canvas_height);
	}
	scene_cache_valid = true;
	scene_cache_dirty = false;
	return true;
}

void VideoDisplay::DrawSceneCache(wxSize const&, int canvas_width, int canvas_height) {
	if (!scene_cache_valid || !scene_cache_texture || canvas_width <= 0 || canvas_height <= 0)
		return;

	BindWindowFramebufferForDisplayRender();
	E(glDisable(GL_SCISSOR_TEST));
	E(glDisable(GL_STENCIL_TEST));
	E(glDisable(GL_CULL_FACE));
	E(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
	E(glDisable(GL_BLEND));
	E(glViewport(0, 0, canvas_width, canvas_height));
	E(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
	E(glClearStencil(0));
	E(glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
	E(glMatrixMode(GL_PROJECTION));
	E(glLoadIdentity());
	E(glOrtho(0.0, canvas_width, 0.0, canvas_height, -1.0, 1.0));
	E(glMatrixMode(GL_MODELVIEW));
	E(glLoadIdentity());
	legacy_gl::DrawTexturedQuad(static_cast<GLuint>(scene_cache_texture), canvas_width, canvas_height);
	if (GLenum err = glGetError())
		throw OpenGlException("legacy_gl::DrawTexturedQuad", err);
}

void VideoDisplay::DrawLegacyOverlayPass(wxSize const& client_size) {
	// Restore legacy-compatible GL state before the overlay pass, since the
	// video backend (libplacebo or modern OpenGL pipeline) may have left VAOs,
	// shader programs, or other non-fixed-function state bound.
	legacy_gl::ResetCompatibilityState();

	// Overlay pass (overscan mask, visual tools) renders in client/window coordinates.
	// Always use a viewport anchored at (0,0) so that ortho coords == mouse coords.
	// The video_pos offset already positions tool features relative to the video.
	E(glViewport(0, 0, client_size.GetWidth() * scale_factor, client_size.GetHeight() * scale_factor));
	E(glMatrixMode(GL_PROJECTION));
	E(glLoadIdentity());
	E(glOrtho(0.0f, client_size.GetWidth(), client_size.GetHeight(), 0.0f, -1000.0f, 1000.0f));
	E(glMatrixMode(GL_MODELVIEW));
	E(glLoadIdentity());

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
}

bool VideoDisplay::TryDrawSkiaOverlayPass(wxSize const& client_size) {
#ifndef WITH_SKIA
	(void)client_size;
	return false;
#else
	if (!IsSkiaVideoOverlayEnabled())
		return false;

	if ((mouse_pos || !autohideTools->GetBool()) && tool && !tool->SupportsOverlayContext())
		return false;

	if (!skia_overlay_context_host)
		skia_overlay_context_host = agi::make_unique<SkiaGpuContextHost>();
	if (!skia_overlay_surface_provider)
		skia_overlay_surface_provider = agi::make_unique<SkiaSurfaceProvider>();
	if (!skia_overlay_text_cache)
		skia_overlay_text_cache = agi::make_unique<SkiaTextLayoutCache>();

	if (!skia_overlay_context_host->EnsureCurrentContext())
		return false;
	skia_overlay_context_host->SyncExternalState();

	int const canvas_width = client_size.GetWidth() * scale_factor;
	int const canvas_height = client_size.GetHeight() * scale_factor;
	if (!EnsureSkiaOverlayBacking(canvas_width, canvas_height))
		return false;

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer)
		return false;

	GLint previous_framebuffer = 0;
	GLint previous_draw_buffer = GL_BACK;
	GLint previous_read_buffer = GL_BACK;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &previous_framebuffer);
	glGetIntegerv(GL_DRAW_BUFFER, &previous_draw_buffer);
	glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
	auto restore_framebuffer = agi::make_scope_exit([&] {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(previous_framebuffer));
		glDrawBuffer(static_cast<GLenum>(previous_draw_buffer));
		glReadBuffer(previous_framebuffer == 0 ? GL_BACK : static_cast<GLenum>(previous_read_buffer));
	});

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, skia_overlay_framebuffer);
	glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

	SkiaFramebufferSurfaceDescriptor descriptor;
	descriptor.width = canvas_width;
	descriptor.height = canvas_height;
	descriptor.sample_count = 0;
	descriptor.stencil_bits = 8;
	descriptor.framebuffer_id = skia_overlay_framebuffer;
	descriptor.bottom_left_origin = false;
	auto surface = skia_overlay_surface_provider->AcquireFramebufferSurface(
		skia_overlay_context_host->Get(),
		descriptor);
	if (!surface)
		return false;

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, skia_overlay_invert_framebuffer);
	glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

	SkiaFramebufferSurfaceDescriptor invert_descriptor = descriptor;
	invert_descriptor.framebuffer_id = skia_overlay_invert_framebuffer;
	auto invert_surface = skia_overlay_surface_provider->AcquireFramebufferSurface(
		skia_overlay_context_host->Get(),
		invert_descriptor);
	if (!invert_surface)
		return false;

	SkCanvas *canvas = surface.get()->getCanvas();
	SkCanvas *invert_canvas = invert_surface.get()->getCanvas();
	if (!canvas)
		return false;
	if (!invert_canvas)
		return false;

	canvas->clear(SK_ColorTRANSPARENT);
	invert_canvas->clear(SK_ColorTRANSPARENT);
	canvas->save();
	invert_canvas->save();
	canvas->scale(scale_factor, scale_factor);
	invert_canvas->scale(scale_factor, scale_factor);

	if (OPT_GET("Video/Overscan Mask")->GetBool()) {
		double ar = con->videoController->GetAspectRatioValue();
		if (ar > 1.75) {
			DrawOverscanMaskSkia(*canvas, .1f, .05f);
			DrawOverscanMaskSkia(*canvas, 0.035f, 0.035f);
		}
		else {
			DrawOverscanMaskSkia(*canvas, 0.067f, 0.05f);
			DrawOverscanMaskSkia(*canvas, 0.033f, 0.035f);
		}
	}

	if ((mouse_pos || !autohideTools->GetBool()) && tool) {
		SkiaVideoOverlayDrawContext draw_context(*canvas, invert_canvas, *skia_overlay_text_cache, static_cast<float>(scale_factor));
		tool->DrawOverlay(draw_context);
	}

	canvas->restore();
	invert_canvas->restore();
	skia_overlay_context_host->FlushAndSubmit();
	skia_overlay_context_host->ResetTextureBindingsForExternalUse();

	// Composite the Skia-drawn overlay texture back to the window framebuffer.
	// The Skia surface uses kTopLeft origin, so the texture content is already
	// in top-left layout. Use a top-left ortho projection with normal (non-flipped)
	// tex coords so the overlay composites correctly.
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(previous_framebuffer));
	glDrawBuffer(static_cast<GLenum>(previous_draw_buffer));
	glReadBuffer(previous_framebuffer == 0 ? GL_BACK : static_cast<GLenum>(previous_read_buffer));
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(0, 0, canvas_width, canvas_height);
	legacy_gl::DrawPremultipliedTexturedQuadTopLeft(static_cast<GLuint>(skia_overlay_texture), canvas_width, canvas_height);
	legacy_gl::DrawAlphaMaskedInvertQuadTopLeft(static_cast<GLuint>(skia_overlay_invert_texture), canvas_width, canvas_height);
	return true;
#endif
}

void VideoDisplay::DrawOverlayPass(wxSize const& client_size) {
	if (!TryDrawSkiaOverlayPass(client_size))
		DrawLegacyOverlayPass(client_size);
}

void VideoDisplay::RefreshDisplayedSubtitleSceneSnapshot() {
	displayed_subtitle_scene.clear();

	if (!has_displayed_packet)
		return;

	auto *subs = con->ass.get();
	auto *project = con->project.get();
	if (!subs || !project)
		return;

	auto const frame_time = static_cast<int>(displayed_packet.time);
	auto const& fps = project->Timecodes();
	displayed_subtitle_scene = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(
		subs->Events,
		fps,
		frame_time);
}

void VideoDisplay::OnSubtitlesCommit(int type, AssDialogue const* changed) {
	if (!video_subtitle_scene_cache::IsVisualSubtitleCommitType(type))
		return;

	auto render_after_commit = [&] {
		if (con->videoController->IsPlaying() && tool && tool->IsInteracting())
			return;
		Render();
	};

	if (!has_displayed_packet) {
		if (!last_frame_had_separate_overlay)
			InvalidateSceneCache();
		scene_cache_waiting_for_subtitle_packet = false;
		return;
	}

	auto *subs = con->ass.get();
	auto *project = con->project.get();
	if (!subs || !project) {
		if (!last_frame_had_separate_overlay)
			InvalidateSceneCache();
		scene_cache_waiting_for_subtitle_packet = true;
		render_after_commit();
		return;
	}

	bool const keep_interactive_cache = tool
		&& tool->IsInteracting()
		&& !last_frame_had_separate_overlay
		&& ShouldUseSceneCacheForCurrentFrame();

	scene_cache_waiting_for_subtitle_packet = !keep_interactive_cache
		&& video_subtitle_scene_cache::ShouldWaitForFreshPacket(
			type,
			has_displayed_packet,
			changed != nullptr,
			subs->Events,
			project->Timecodes(),
			static_cast<int>(displayed_packet.time),
			displayed_subtitle_scene);

	// Integrated subtitle rendering bakes the subtitle layer into the cached
	// scene, so any visual commit invalidates the cache. In separate-overlay
	// mode the cache is video-only and remains reusable; the wait flag above
	// simply blocks reuse until a fresh overlay packet arrives when needed.
	if (!last_frame_had_separate_overlay && !keep_interactive_cache)
		InvalidateSceneCache();
	render_after_commit();
}

void VideoDisplay::DoRender() try {
	if (render_in_progress) {
		render_requested = true;
		ScheduleRender();
		return;
	}

	render_in_progress = true;
	render_scheduled = false;
	auto finish_render = agi::make_scope_exit([&] {
		render_in_progress = false;
		if (render_requested)
			ScheduleRender();
	});
	render_requested = false;

	if (!con->project->VideoProvider() || !InitContext() || (!videoRenderer && !has_pending_packet))
		return;

	bool presented_new_frame = false;
	int presented_frame_number = -1;
	bool first_presented_frame = false;

	bool renderer_was_just_created = false;
	if (!videoRenderer) {
		auto renderer_result = CreateConfiguredVideoRenderer();
		videoRenderer = std::move(renderer_result.renderer);
		renderer_was_just_created = true;
		if (ApplyRendererSourceModePreference()) {
			pending_packet = { };
			has_pending_packet = false;
			pending_packet_deferred_for_visual_interaction = false;
			displayed_packet = { };
			has_displayed_packet = false;
			con->videoController->JumpToFrame(con->videoController->GetFrameN());
			return;
		}
	}

	if (!tool)
		cmd::call("video/tool/cross", con);

	try {
		if (has_pending_packet && !(pending_packet_deferred_for_visual_interaction && tool && tool->IsInteracting())) {
			first_presented_frame = !has_displayed_packet;
			bool const reuse_uploaded_source_frame =
				pending_packet.allow_source_frame_upload_reuse
				&&
				!renderer_was_just_created
				&& has_displayed_packet
				&& displayed_packet.allow_source_frame_upload_reuse
				&& pending_packet.frame_number == displayed_packet.frame_number
				&& SourceFrameEquivalentForUpload(pending_packet.source_frame, displayed_packet.source_frame);
			auto const routing = DecideVideoRenderRouting(
				pending_packet,
				videoRenderer->SupportsDirectOverlay());
			last_frame_had_separate_overlay = (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay);

			if (routing == VideoRenderRoutingMode::SourceFrameOnly) {
				if (!reuse_uploaded_source_frame)
					videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(nullptr);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			else if (routing == VideoRenderRoutingMode::PrimaryRendererDirectOverlay) {
				if (!reuse_uploaded_source_frame)
					videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(&pending_packet.subtitle_overlay);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			else if (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay) {
				if (!reuse_uploaded_source_frame)
					videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(nullptr);
				bool const created_overlay_renderer = !subtitleOverlayRenderer;
				if (created_overlay_renderer)
					subtitleOverlayRenderer = agi::make_unique<OpenGLVideoRenderer>(false, true, false);
				if (!reuse_uploaded_source_frame || created_overlay_renderer)
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
			pending_packet_deferred_for_visual_interaction = false;
			RefreshDisplayedSubtitleSceneSnapshot();
			presented_new_frame = true;
			presented_frame_number = displayed_packet.frame_number;
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

	wxSize client_size = GetClientSize();
	client_size = wxSize(std::max(1, client_size.GetWidth()), std::max(1, client_size.GetHeight()));
	int const canvas_width = client_size.GetWidth() * scale_factor;
	int const canvas_height = client_size.GetHeight() * scale_factor;

	bool const skip_scene_cache_for_renderer_warmup = first_presented_frame || renderer_was_just_created;

	bool rendered_from_scene_cache = false;
	if (!skip_scene_cache_for_renderer_warmup
		&& !scene_cache_waiting_for_subtitle_packet
		&& IsSceneCacheUsableForCurrentPlayback()
		&& ShouldAttemptSceneCache(canvas_width, canvas_height)) {
		try {
			if (scene_cache_dirty
				|| !scene_cache_valid
				|| scene_cache_width != canvas_width
				|| scene_cache_height != canvas_height) {
				if (!RenderSceneToCache(client_size, canvas_width, canvas_height)) {
					LOG_W("video/display/scene_cache")
						<< "Video scene cache could not be created for canvas "
						<< canvas_width << "x" << canvas_height
						<< "; falling back to direct backend rendering until the display size changes or the renderer resets.";
					BlockSceneCacheUntilRetry(canvas_width, canvas_height);
				}
			}
			if (!scene_cache_retry_blocked) {
				DrawSceneCache(client_size, canvas_width, canvas_height);
				rendered_from_scene_cache = true;
			}
		}
		catch (agi::Exception const& err) {
			LOG_W("video/display/scene_cache")
				<< "Video scene cache failed for canvas "
				<< canvas_width << "x" << canvas_height
				<< "; falling back to direct backend rendering until the display size changes or the renderer resets: "
				<< err.GetMessage();
			BlockSceneCacheUntilRetry(canvas_width, canvas_height);
		}
	}
	if (!rendered_from_scene_cache) {
		BindWindowFramebufferForDisplayRender();
		RenderBackendScene(canvas_width, canvas_height);
		scene_cache_valid = false;
		scene_cache_dirty = true;
	}

	// When the scene cache is video-only (separate overlay mode), the subtitle
	// overlay pass was skipped during cache fill and must be applied now.
	if (rendered_from_scene_cache && last_frame_had_separate_overlay && subtitleOverlayRenderer) {
		BindWindowFramebufferForDisplayRender();
		subtitleOverlayRenderer->Render(
			{ viewport_left, viewport_bottom, viewport_width, viewport_height },
			canvas_width,
			canvas_height);
	}

	DrawOverlayPass(client_size);

	SwapBuffers();

	if (presented_new_frame) {
		FramePresented(presented_frame_number);
		con->videoController->NotifyFramePresented(presented_frame_number);
		if (perf_trace::ShouldSampleVideoMemory(first_presented_frame)) {
			auto snapshot = BuildVideoMemorySnapshot(con, this);
			perf_trace::ObserveVideoMemorySnapshot("frame_presented", snapshot, first_presented_frame);
		}
	}
}
catch (const agi::Exception &err) {
	wxLogError(
		wxS("An error occurred trying to render the video frame on the screen.\n"
		    "Error message reported: %s"),
		to_wx(err.GetMessage()));
	con->project->CloseVideo();
}

void VideoDisplay::DrawOverscanMask(float horizontal_percent, float vertical_percent) const {
	// This pass renders in logical client coordinates, so keep the clip rect and
	// mask geometry in the same space on HiDPI displays.
	Vector2D viewport_pos = Vector2D(viewport_left, viewport_top) / scale_factor;
	Vector2D viewport_size = Vector2D(viewport_width, viewport_height) / scale_factor;
	Vector2D v = viewport_size;
	Vector2D size = Vector2D(horizontal_percent, vertical_percent) / 2 * v;

	// Clockwise from top-left
	Vector2D corners[] = {
		size,
		Vector2D(viewport_size.X() - size.X(), size),
		v - size,
		Vector2D(size, viewport_size.Y() - size.Y())
	};

	// Shift to compensate for black bars
	for (auto& corner : corners)
		corner = corner + viewport_pos;

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
	gl.DrawMultiPolygon(points, vstart, vcount, viewport_pos, viewport_size, true);
}

#ifdef WITH_SKIA
void VideoDisplay::DrawOverscanMaskSkia(SkCanvas &canvas, float horizontal_percent, float vertical_percent) const {
	Vector2D viewport_pos = Vector2D(viewport_left, viewport_top) / scale_factor;
	Vector2D viewport_size = Vector2D(viewport_width, viewport_height) / scale_factor;
	Vector2D const size = Vector2D(horizontal_percent, vertical_percent) / 2 * viewport_size;

	Vector2D corners[] = {
		size,
		Vector2D(viewport_size.X() - size.X(), size),
		viewport_size - size,
		Vector2D(size, viewport_size.Y() - size.Y())
	};

	for (auto& corner : corners)
		corner = corner + viewport_pos;

	std::vector<float> points;
	for (size_t i = 0; i < 4; ++i) {
		size_t const prev = (i + 3) % 4;
		size_t const next = (i + 1) % 4;
		SplineCurve(
			(corners[prev] + corners[i] * 4) / 5,
			corners[i], corners[i],
			(corners[next] + corners[i] * 4) / 5)
			.GetPoints(points);
	}

	SkPathBuilder builder(SkPathFillType::kEvenOdd);
	builder.addRect(SkRect::MakeXYWH(viewport_pos.X(), viewport_pos.Y(), viewport_size.X(), viewport_size.Y()));
	if (!points.empty()) {
		std::vector<SkPoint> polygon;
		polygon.reserve(points.size() / 2);
		for (size_t i = 0; i + 1 < points.size(); i += 2)
			polygon.push_back(SkPoint::Make(points[i], points[i + 1]));
		builder.addPolygon({ polygon.data(), polygon.size() }, true);
	}
	SkPath const path = builder.detach();

	SkPaint fill;
	fill.setAntiAlias(true);
	fill.setStyle(SkPaint::kFill_Style);
	fill.setColor(SkColorSetARGB(128, 30, 70, 200));
	canvas.drawPath(path, fill);
}

bool VideoDisplay::EnsureSkiaOverlayBacking(int canvas_width, int canvas_height) {
	if (canvas_width <= 0 || canvas_height <= 0)
		return false;

	if (skia_overlay_framebuffer
		&& skia_overlay_texture
		&& skia_overlay_stencil_renderbuffer
		&& skia_overlay_invert_framebuffer
		&& skia_overlay_invert_texture
		&& skia_overlay_invert_stencil_renderbuffer
		&& canvas_width == skia_overlay_width
		&& canvas_height == skia_overlay_height) {
		return true;
	}

	DestroySkiaOverlayBacking();

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.GenFramebuffers
		|| !gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus
		|| !gl.GenRenderbuffers
		|| !gl.BindRenderbuffer
		|| !gl.DeleteRenderbuffers
		|| !gl.RenderbufferStorage
		|| !gl.FramebufferRenderbuffer) {
		return false;
	}

	ScopedFramebufferState restore_state(gl);

	glGenTextures(1, &skia_overlay_texture);
	glBindTexture(GL_TEXTURE_2D, skia_overlay_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA8,
		canvas_width,
		canvas_height,
		0,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	gl.GenFramebuffers(1, &skia_overlay_framebuffer);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, skia_overlay_framebuffer);
	gl.FramebufferTexture2D(
		GL_FRAMEBUFFER_EXT,
		GL_COLOR_ATTACHMENT0_EXT,
		GL_TEXTURE_2D,
		skia_overlay_texture,
		0);

	gl.GenRenderbuffers(1, &skia_overlay_stencil_renderbuffer);
	gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, skia_overlay_stencil_renderbuffer);
	gl.RenderbufferStorage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, canvas_width, canvas_height);
	gl.FramebufferRenderbuffer(
		GL_FRAMEBUFFER_EXT,
		GL_DEPTH_ATTACHMENT_EXT,
		GL_RENDERBUFFER_EXT,
		skia_overlay_stencil_renderbuffer);
	gl.FramebufferRenderbuffer(
		GL_FRAMEBUFFER_EXT,
		GL_STENCIL_ATTACHMENT_EXT,
		GL_RENDERBUFFER_EXT,
		skia_overlay_stencil_renderbuffer);
	gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, 0);

	GLenum const status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
	if (status != GL_FRAMEBUFFER_COMPLETE && status != GL_FRAMEBUFFER_COMPLETE_EXT) {
		DestroySkiaOverlayBacking();
		return false;
	}

	glGenTextures(1, &skia_overlay_invert_texture);
	glBindTexture(GL_TEXTURE_2D, skia_overlay_invert_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA8,
		canvas_width,
		canvas_height,
		0,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	gl.GenFramebuffers(1, &skia_overlay_invert_framebuffer);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, skia_overlay_invert_framebuffer);
	gl.FramebufferTexture2D(
		GL_FRAMEBUFFER_EXT,
		GL_COLOR_ATTACHMENT0_EXT,
		GL_TEXTURE_2D,
		skia_overlay_invert_texture,
		0);

	gl.GenRenderbuffers(1, &skia_overlay_invert_stencil_renderbuffer);
	gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, skia_overlay_invert_stencil_renderbuffer);
	gl.RenderbufferStorage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, canvas_width, canvas_height);
	gl.FramebufferRenderbuffer(
		GL_FRAMEBUFFER_EXT,
		GL_DEPTH_ATTACHMENT_EXT,
		GL_RENDERBUFFER_EXT,
		skia_overlay_invert_stencil_renderbuffer);
	gl.FramebufferRenderbuffer(
		GL_FRAMEBUFFER_EXT,
		GL_STENCIL_ATTACHMENT_EXT,
		GL_RENDERBUFFER_EXT,
		skia_overlay_invert_stencil_renderbuffer);
	gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, 0);

	GLenum const invert_status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
	if (invert_status != GL_FRAMEBUFFER_COMPLETE && invert_status != GL_FRAMEBUFFER_COMPLETE_EXT) {
		DestroySkiaOverlayBacking();
		return false;
	}

	skia_overlay_width = canvas_width;
	skia_overlay_height = canvas_height;
	return true;
}

void VideoDisplay::DestroySkiaOverlayBacking() noexcept {
	// Release Skia GPU resources first so Skia drops any internal references
	// to the GL objects we are about to delete.
	if (skia_overlay_context_host)
		skia_overlay_context_host->FlushAndSubmit();

	auto const& gl = GetCaptureFramebufferFunctions();

	if (skia_overlay_framebuffer) {
		if (gl.DeleteFramebuffers)
			gl.DeleteFramebuffers(1, &skia_overlay_framebuffer);
		skia_overlay_framebuffer = 0;
	}
	if (skia_overlay_invert_framebuffer) {
		if (gl.DeleteFramebuffers)
			gl.DeleteFramebuffers(1, &skia_overlay_invert_framebuffer);
		skia_overlay_invert_framebuffer = 0;
	}
	if (skia_overlay_stencil_renderbuffer) {
		if (gl.DeleteRenderbuffers)
			gl.DeleteRenderbuffers(1, &skia_overlay_stencil_renderbuffer);
		skia_overlay_stencil_renderbuffer = 0;
	}
	if (skia_overlay_invert_stencil_renderbuffer) {
		if (gl.DeleteRenderbuffers)
			gl.DeleteRenderbuffers(1, &skia_overlay_invert_stencil_renderbuffer);
		skia_overlay_invert_stencil_renderbuffer = 0;
	}
	if (skia_overlay_texture) {
		glDeleteTextures(1, &skia_overlay_texture);
		skia_overlay_texture = 0;
	}
	if (skia_overlay_invert_texture) {
		glDeleteTextures(1, &skia_overlay_invert_texture);
		skia_overlay_invert_texture = 0;
	}

	skia_overlay_width = 0;
	skia_overlay_height = 0;
}
#endif

void VideoDisplay::PositionVideo() {
	auto provider = con->project->VideoProvider();
	if (!provider || !IsShownOnScreen()) return;

	int const canvas_width = GetClientSize().GetWidth() * scale_factor;
	int const canvas_height = GetClientSize().GetHeight() * scale_factor;

	AspectRatio arType = con->videoController->GetAspectRatioType();
	double target_aspect_ratio = 0.0;
	if (freeSize) {
		target_aspect_ratio = arType == AspectRatio::Default
			? static_cast<double>(provider->GetWidth()) / provider->GetHeight()
			: con->videoController->GetAspectRatioValue();
	}
	baseViewport = BuildVideoDisplayViewportLayout(
		canvas_width,
		canvas_height,
		videoSize.GetWidth(),
		videoSize.GetHeight(),
		freeSize,
		target_aspect_ratio);
	bool const enable_content_transform =
		(!freeSize && contentZoomValue != 1.0) ||
		pan_x != 0.0 ||
		pan_y != 0.0;
	auto layout = BuildVideoDisplayContentLayout(
		baseViewport,
		canvas_height,
		enable_content_transform,
		{ freeSize ? 1.0 : contentZoomValue, pan_x, pan_y });
	ApplyViewportLayout(layout, viewport_left, viewport_width, viewport_bottom, viewport_top, viewport_height);

	if (tool) {
		wxSize client_size = GetClientSize();
		tool->SetCanvasSize(client_size.GetWidth(), client_size.GetHeight());
		tool->SetDisplayArea(viewport_left / scale_factor, viewport_top / scale_factor,
			viewport_width / scale_factor, viewport_height / scale_factor);
	}

	InvalidateSceneCache();
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

		contentZoomValue = 1.0;
		pan_x = 0.0;
		pan_y = 0.0;

		wxSize cs = GetClientSize();
		wxSize oldSize = top->GetSize();
		top->SetSize(top->GetSize() + videoSize / scale_factor - cs);
		SetClientSize(cs + top->GetSize() - oldSize);
	}
	else {
		SetMinClientSize(videoSize / scale_factor);
		SetMaxClientSize(videoSize / scale_factor);

		if (auto frame = con->GetUI().frame)
			frame->UpdateEditGridSplitterForContentChange();
		else
			LayoutContainingSizers();
	}

	PositionVideo();
}

void VideoDisplay::LayoutContainingSizers() {
	wxWindow *layout_root = GetGrandParent();
	if (!layout_root)
		layout_root = GetParent();
	if (layout_root)
		layout_root->Layout();
}

void VideoDisplay::RefreshVideoScale() {
	if (tool && toolBar) {
		toolBar->ClearTools();
		tool->SetToolbar(toolBar);
		LayoutContainingSizers();
	}
	if (con->project->VideoProvider())
		UpdateSize();
}

void VideoDisplay::OnSizeEvent(wxSizeEvent &event) {
	if (freeSize) {
		wxSize newVideoSize = GetClientSize() * scale_factor;
		// Only reset pan/zoom when the window size actually changed (user resize),
		// not when an internal layout change (e.g. toolbar swap) re-enters here.
		if (newVideoSize != videoSize) {
			contentZoomValue = 1.0;
			pan_x = 0.0;
			pan_y = 0.0;
		}
		videoSize = newVideoSize;
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
	wxPoint pt = event.GetPosition();
	Vector2D current_pos(pt.x, pt.y);

	if (event.ButtonDown())
		SetFocus();

	if (event.MiddleDown())
		last_mouse_pos = current_pos;
	else if (event.Dragging() && event.MiddleIsDown())
		Pan(current_pos - last_mouse_pos);

	last_mouse_pos = mouse_pos = current_pos;

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
			if (freeSize) {
				SetZoom(AdvanceDetachedVideoZoomByWheel(zoomValue, wheel_steps, zoomBox->GetCount()));
				return;
			}

			int action = ResolveVideoDisplayScrollAction(
				event.CmdDown(),
				event.ShiftDown(),
				scrollAction->GetInt(),
				ctrlScrollAction->GetInt(),
				shiftScrollAction->GetInt());
			int dir = 1;
			bool swap = false;
			switch (action) {
				case SCALE_VIDEO_REV:
					dir = -1;
					[[fallthrough]];
				case SCALE_VIDEO:
					SetWindowZoom(zoomValue + dir * kVideoZoomStep * wheel_steps);
					break;

				case ZOOM_VIDEO_REV:
					dir = -1;
					[[fallthrough]];
				case ZOOM_VIDEO:
				{
					double newZoomValue = contentZoomValue * (1 + dir * kVideoZoomStep * wheel_steps);
					wxPoint scaled_position = event.GetPosition() * scale_factor;
					ZoomAndPan(newZoomValue, GetZoomAnchorPoint(scaled_position), scaled_position);
					break;
				}

				case PAN_VIDEO_SWAP:
					swap = true;
					[[fallthrough]];
				case PAN_VIDEO:
				{
					double distance = 5.0 * wheel_steps;
					Vector2D pan = event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL ? Vector2D(-distance, 0) : Vector2D(0, distance);
					Pan(swap ? Vector2D(pan.Y(), pan.X()) : pan);
					break;
				}

				case NOTHING:
				default:
					break;
			}
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
		LayoutContainingSizers();
		tool->SetCanvasSize(GetClientSize().GetWidth(), GetClientSize().GetHeight());
		tool->SetDisplayArea(viewport_left / scale_factor, viewport_top / scale_factor,
			viewport_width / scale_factor, viewport_height / scale_factor);
		Render();
	}
}

void VideoDisplay::Pan(Vector2D delta) {
	if (baseViewport.viewport_height <= 0)
		return;

	auto transform = PanVideoDisplayContent(
		baseViewport,
		{ freeSize ? 1.0 : contentZoomValue, pan_x, pan_y },
		delta * scale_factor);
	contentZoomValue = transform.zoom;
	pan_x = transform.pan_x;
	pan_y = transform.pan_y;
	PositionVideo();
}

Vector2D VideoDisplay::GetZoomAnchorPoint(wxPoint position) const {
	if (freeSize)
		return {};

	return ::GetVideoDisplayZoomAnchorPoint(
		baseViewport,
		{ contentZoomValue, pan_x, pan_y },
		Vector2D(position.x, position.y));
}

void VideoDisplay::ZoomAndPan(double newZoomValue, Vector2D anchorPoint, wxPoint newPosition) {
	if (freeSize)
		return;

	auto transform = ::ZoomVideoDisplayContent(
		baseViewport,
		{ contentZoomValue, pan_x, pan_y },
		newZoomValue,
		anchorPoint,
		Vector2D(newPosition.x, newPosition.y));
	contentZoomValue = transform.zoom;
	pan_x = transform.pan_x;
	pan_y = transform.pan_y;
	PositionVideo();
}

void VideoDisplay::ResetContentZoom() {
	contentZoomValue = 1.0;
	pan_x = 0.0;
	pan_y = 0.0;
	PositionVideo();
}

bool VideoDisplay::ToolIsType(std::type_info const& type) const {
	return tool && typeid(*tool) == type;
}

Vector2D VideoDisplay::GetMousePosition() const {
	return last_mouse_pos ? tool->ToScriptCoords(last_mouse_pos) : last_mouse_pos;
}

void VideoDisplay::Unload() {
	ResetRenderers();
	if (glContext)
		SetCurrent(*glContext);
	DestroySceneCache();
#ifdef WITH_SKIA
	DestroySkiaOverlayBacking();
	skia_overlay_context_host.reset();
	skia_overlay_surface_provider.reset();
	skia_overlay_text_cache.reset();
#endif
	tool.reset();
	glContext.reset();
	pending_packet = { };
	has_pending_packet = false;
	pending_packet_deferred_for_visual_interaction = false;
	displayed_packet = { };
	has_displayed_packet = false;
	ResetDisplayedSubtitleScene();
}
