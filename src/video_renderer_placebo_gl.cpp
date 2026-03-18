// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "video_renderer_placebo_gl.h"

#include "subtitle_overlay.h"
#include "video_renderer_error.h"
#include "video_renderer_placebo_runtime.h"

#include <libaegisub/log.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include <GL/glx.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace {
constexpr char kPlaceboLogTag[] = "video/out/placebo";
constexpr std::array<uint64_t, 4> kBgra8Masks = {
	0x00ff0000ull,
	0x0000ff00ull,
	0x000000ffull,
	0xff000000ull
};

void LogInfo(std::string const& message) {
	if (agi::log::log)
		LOG_I(kPlaceboLogTag) << message;
}

void PlaceboLogCallback(void *, enum pl_log_level level, const char *msg) {
	if (!agi::log::log)
		return;

	switch (level) {
		case PL_LOG_FATAL:
		case PL_LOG_ERR:
			LOG_E(kPlaceboLogTag) << msg;
			break;
		case PL_LOG_WARN:
			LOG_W(kPlaceboLogTag) << msg;
			break;
		case PL_LOG_INFO:
			LOG_I(kPlaceboLogTag) << msg;
			break;
		case PL_LOG_DEBUG:
		case PL_LOG_TRACE:
			LOG_D(kPlaceboLogTag) << msg;
			break;
		default:
			break;
	}
}

std::string NormalizeColorToken(std::string value) {
	std::string normalized;
	normalized.reserve(value.size());
	for (unsigned char ch : value) {
		if (std::isalnum(ch))
			normalized.push_back(static_cast<char>(std::toupper(ch)));
	}
	return normalized;
}

bool ContainsToken(std::string const& value, char const *token) {
	return value.find(token) != std::string::npos;
}

enum pl_color_primaries InferPrimaries(SourceFrameColorMetadata const& color) {
	auto token = NormalizeColorToken(color.primaries);
	if (ContainsToken(token, "2020"))
		return PL_COLOR_PRIM_BT_2020;
	if (ContainsToken(token, "470M"))
		return PL_COLOR_PRIM_BT_470M;
	if (ContainsToken(token, "601525") || ContainsToken(token, "170M") || ContainsToken(token, "240M"))
		return PL_COLOR_PRIM_BT_601_525;
	if (ContainsToken(token, "601625") || ContainsToken(token, "470BG") || ContainsToken(token, "BT601"))
		return PL_COLOR_PRIM_BT_601_625;
	if (ContainsToken(token, "DISPLAYP3"))
		return PL_COLOR_PRIM_DISPLAY_P3;
	if (ContainsToken(token, "DCIP3"))
		return PL_COLOR_PRIM_DCI_P3;
	return PL_COLOR_PRIM_BT_709;
}

enum pl_color_transfer InferTransfer(SourceFrameColorMetadata const& color) {
	auto token = NormalizeColorToken(color.transfer);
	if (ContainsToken(token, "LINEAR"))
		return PL_COLOR_TRC_LINEAR;
	if (ContainsToken(token, "SRGB"))
		return PL_COLOR_TRC_SRGB;
	if (ContainsToken(token, "PQ") || ContainsToken(token, "2084"))
		return PL_COLOR_TRC_PQ;
	if (ContainsToken(token, "HLG"))
		return PL_COLOR_TRC_HLG;
	return PL_COLOR_TRC_BT_1886;
}

enum pl_color_levels InferLevels(SourceFrameColorMetadata const& color) {
	if (color.range == SourceFrameColorRange::Limited)
		return PL_COLOR_LEVELS_LIMITED;
	return PL_COLOR_LEVELS_FULL;
}

struct pl_color_repr BuildImageRepr(SourceFrameColorMetadata const& color) {
	struct pl_color_repr repr = {};
	repr.sys = PL_COLOR_SYSTEM_RGB;
	repr.levels = InferLevels(color);
	repr.alpha = PL_ALPHA_NONE;
	repr.bits = { 8, 8, 0 };
	return repr;
}

struct pl_color_space BuildImageColorSpace(SourceFrameColorMetadata const& color) {
	struct pl_color_space space = {};
	space.primaries = InferPrimaries(color);
	space.transfer = InferTransfer(color);
	return space;
}

pl_rect2df FullRect(int width, int height) {
	return { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
}

pl_rect2df ViewportRect(RenderViewport const& viewport, int canvas_width, int canvas_height) {
	float const top = static_cast<float>(canvas_height - viewport.y - viewport.height);
	return {
		static_cast<float>(viewport.x),
		top,
		static_cast<float>(viewport.x + viewport.width),
		top + static_cast<float>(viewport.height)
	};
}

void *GetGLProcAddress(char const *name) {
#ifdef _WIN32
	void *proc = reinterpret_cast<void *>(wglGetProcAddress(name));
	if (proc == nullptr || proc == reinterpret_cast<void *>(0x1) || proc == reinterpret_cast<void *>(0x2)
		|| proc == reinterpret_cast<void *>(0x3) || proc == reinterpret_cast<void *>(-1))
		return nullptr;
	return proc;
#elif defined(__APPLE__)
	return dlsym(RTLD_DEFAULT, name);
#else
	return reinterpret_cast<void *>(glXGetProcAddress(reinterpret_cast<GLubyte const *>(name)));
#endif
}

template <typename Proc>
Proc LoadOptionalProc(char const *name, char const *fallback_name = nullptr) {
	if (auto *proc = GetGLProcAddress(name))
		return reinterpret_cast<Proc>(proc);
	if (fallback_name) {
		if (auto *proc = GetGLProcAddress(fallback_name))
			return reinterpret_cast<Proc>(proc);
	}
	return nullptr;
}
}

struct PlaceboRendererGL::Functions {
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
};

PlaceboRendererGL::PlaceboRendererGL() {
	try {
		EnsureInitialized();
	}
	catch (...) {
		DestroyResources();
		throw;
	}
}

PlaceboRendererGL::~PlaceboRendererGL() {
	DestroyResources();
}

void PlaceboRendererGL::EnsureInitialized() {
	if (renderer)
		return;

	try {
		placebo::runtime::EnsureLoaded();
		api = &placebo::runtime::GetApi();

		struct pl_log_params log_params = {};
		log_params.log_cb = &PlaceboLogCallback;
		log_params.log_level = PL_LOG_INFO;
		log = api->log_create(PL_API_VER, &log_params);
		if (!log)
			throw VideoOutInitException("Failed to create libplacebo log.");

		opengl = api->opengl_create(log, nullptr);
		if (!opengl)
			throw VideoOutInitException("Failed to create libplacebo OpenGL context wrapper.");

		renderer = api->renderer_create(log, opengl->gpu);
		if (!renderer)
			throw VideoOutInitException("Failed to create libplacebo renderer.");

		functions = std::make_unique<Functions>();
		functions->UseProgram = LoadOptionalProc<PFNGLUSEPROGRAMPROC>("glUseProgram");
		functions->BindFramebuffer = LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT");

		LogInfo("Activated libplacebo video renderer using "
			+ placebo::runtime::GetLoadedLibrary()
			+ " (runtime " + placebo::runtime::GetLoadedVersion()
			+ ", fix " + std::to_string(placebo::runtime::GetLoadedFixVersion()) + ")");
	}
	catch (...) {
		DestroyResources();
		throw;
	}
}

void PlaceboRendererGL::DestroyImageResources() noexcept {
	if (api && opengl && image_texture)
		api->tex_destroy(opengl->gpu, &image_texture);
	image_texture = nullptr;
	image_width = 0;
	image_height = 0;
	image_components = 0;
	image_component_mapping = { { -1, -1, -1, -1 } };
	image_flipped = false;
	image_color = {};
	has_frame = false;
}

void PlaceboRendererGL::DestroyTargetResources() noexcept {
	if (api && opengl && target_texture)
		api->tex_destroy(opengl->gpu, &target_texture);
	target_texture = nullptr;
	target_width = 0;
	target_height = 0;
}

void PlaceboRendererGL::DestroyResources() noexcept {
	DestroyTargetResources();
	DestroyImageResources();

	if (api && renderer)
		api->renderer_destroy(&renderer);
	renderer = nullptr;

	if (api && opengl)
		api->opengl_destroy(&opengl);
	opengl = nullptr;

	if (api && log)
		api->log_destroy(&log);
	log = nullptr;

	functions.reset();
	api = nullptr;
}

void PlaceboRendererGL::Reset() {
	DestroyResources();
}

void PlaceboRendererGL::RecreateTargetTexture(int canvas_width, int canvas_height) {
	if (target_texture && target_width == canvas_width && target_height == canvas_height)
		return;

	DestroyTargetResources();

	struct pl_opengl_wrap_params params = {};
	params.framebuffer = 0;
	params.width = canvas_width;
	params.height = canvas_height;
	target_texture = api->opengl_wrap(opengl->gpu, &params);
	if (!target_texture)
		throw VideoOutInitException("Failed to wrap the current OpenGL framebuffer for libplacebo rendering.");

	target_width = canvas_width;
	target_height = canvas_height;
}

void PlaceboRendererGL::UploadFrame(SourceFrame const& frame) {
	if (!frame.IsValid()) {
		DestroyImageResources();
		return;
	}
	if (frame.pixel_format != SourceFramePixelFormat::Bgra8)
		throw VideoOutRenderException("PlaceboRendererGL currently only supports BGRA8 source frames.");

	EnsureInitialized();

	struct pl_plane_data data = {};
	data.type = PL_FMT_UNORM;
	data.width = frame.width;
	data.height = frame.height;
	data.pixel_stride = 4;
	data.row_stride = static_cast<size_t>(frame.planes[0].stride);
	data.pixels = frame.planes[0].data;
	uint64_t masks[4] = {
		kBgra8Masks[0],
		kBgra8Masks[1],
		kBgra8Masks[2],
		kBgra8Masks[3]
	};
	api->plane_data_from_mask(&data, masks);

	struct pl_plane plane = {};
	if (!api->upload_plane(opengl->gpu, &plane, &image_texture, &data)) {
		DestroyImageResources();
		throw VideoOutRenderException("libplacebo failed to upload the BGRA source frame.");
	}

	plane.flipped = frame.flipped;
	plane.address_mode = PL_TEX_ADDRESS_CLAMP;

	image_width = frame.width;
	image_height = frame.height;
	image_components = plane.components;
	for (int i = 0; i < 4; ++i)
		image_component_mapping[static_cast<size_t>(i)] = plane.component_mapping[i];
	image_flipped = plane.flipped;
	image_color = frame.color;
	has_frame = true;
}

void PlaceboRendererGL::UploadOverlay(SubtitleOverlay const*) {
}

void PlaceboRendererGL::RestoreCompatibilityState() noexcept {
	if (functions) {
		if (functions->UseProgram)
			functions->UseProgram(0);
		if (functions->BindFramebuffer)
			functions->BindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
}

void PlaceboRendererGL::Render(RenderViewport const& viewport, int canvas_width, int canvas_height) {
	if (!has_frame || viewport.width <= 0 || viewport.height <= 0)
		return;

	EnsureInitialized();
	RecreateTargetTexture(canvas_width, canvas_height);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	struct pl_plane image_plane = {};
	image_plane.texture = image_texture;
	image_plane.flipped = image_flipped;
	image_plane.address_mode = PL_TEX_ADDRESS_CLAMP;
	image_plane.components = image_components;
	for (int i = 0; i < 4; ++i)
		image_plane.component_mapping[i] = image_component_mapping[static_cast<size_t>(i)];

	struct pl_frame image = {};
	image.num_planes = 1;
	image.planes[0] = image_plane;
	image.repr = BuildImageRepr(image_color);
	image.color = BuildImageColorSpace(image_color);
	image.crop = FullRect(image_width, image_height);
	image.rotation = PL_ROTATION_0;

	struct pl_plane target_plane = {};
	target_plane.texture = target_texture;
	target_plane.flipped = true;
	target_plane.address_mode = PL_TEX_ADDRESS_CLAMP;
	target_plane.components = 4;
	target_plane.component_mapping[0] = PL_CHANNEL_R;
	target_plane.component_mapping[1] = PL_CHANNEL_G;
	target_plane.component_mapping[2] = PL_CHANNEL_B;
	target_plane.component_mapping[3] = PL_CHANNEL_A;

	struct pl_frame target = {};
	target.num_planes = 1;
	target.planes[0] = target_plane;
	target.repr = image.repr;
	target.color = image.color;
	target.crop = ViewportRect(viewport, canvas_width, canvas_height);
	target.rotation = PL_ROTATION_0;

	struct pl_render_params params = {};
	if (!api->render_image(renderer, &image, &target, &params)) {
		RestoreCompatibilityState();
		throw VideoOutRenderException("libplacebo failed to render the video frame.");
	}

	RestoreCompatibilityState();
}
