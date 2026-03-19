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
#include "video_renderer_placebo_source_frame.h"

#include <libaegisub/log.h>

#include <array>
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

bool BuildBgra8PlaneData(placebo::runtime::Api const& api, SourceFrame const& frame, struct pl_plane_data& data) {
	if (!frame.IsValid()
		|| frame.output_mode != SourceFrameOutputMode::Bgra8
		|| frame.pixel_format != SourceFramePixelFormat::Bgra8)
		return false;

	auto const& plane = frame.planes[0];
	auto* pixels = plane.data;
	ptrdiff_t stride = plane.stride;
	size_t row_stride = static_cast<size_t>(stride < 0 ? -stride : stride);
	if (!pixels || row_stride == 0)
		return false;
	if (stride < 0)
		pixels += static_cast<ptrdiff_t>(plane.height - 1) * row_stride;

	data = {};
	data.type = PL_FMT_UNORM;
	data.width = frame.width;
	data.height = frame.height;
	data.pixel_stride = 4;
	data.row_stride = row_stride;
	data.pixels = pixels;
	uint64_t masks[4] = {
		kBgra8Masks[0],
		kBgra8Masks[1],
		kBgra8Masks[2],
		kBgra8Masks[3]
	};
	api.plane_data_from_mask(&data, masks);
	return true;
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
	for (auto& plane : image_planes) {
		if (api && opengl && plane.texture)
			api->tex_destroy(opengl->gpu, &plane.texture);
		plane = {};
	}
	image_width = 0;
	image_height = 0;
	image_plane_count = 0;
	image_output_mode = SourceFrameOutputMode::Bgra8;
	image_format_info = {};
	image_color = {};
	image_chroma_location = SourceFrameChromaLocation::Unknown;
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

	EnsureInitialized();

	int const next_plane_count = frame.output_mode == SourceFrameOutputMode::Native ? frame.plane_count : 1;
	for (int i = next_plane_count; i < image_plane_count; ++i) {
		auto& plane = image_planes[static_cast<size_t>(i)];
		if (plane.texture)
			api->tex_destroy(opengl->gpu, &plane.texture);
		plane = {};
	}

	for (int i = 0; i < next_plane_count; ++i) {
		struct pl_plane_data data = {};
		bool built = frame.output_mode == SourceFrameOutputMode::Bgra8
			? BuildBgra8PlaneData(*api, frame, data)
			: BuildPlaceboNativePlaneData(frame, i, data);
		if (!built) {
			DestroyImageResources();
			throw VideoOutRenderException("libplacebo could not describe the source frame for upload.");
		}
		if (frame.output_mode == SourceFrameOutputMode::Native && api->plane_data_align) {
			struct pl_bit_encoding ignored_bits = {};
			api->plane_data_align(&data, &ignored_bits);
		}

		struct pl_plane uploaded_plane = {};
		auto& plane_state = image_planes[static_cast<size_t>(i)];
		if (!api->upload_plane(opengl->gpu, &uploaded_plane, &plane_state.texture, &data)) {
			DestroyImageResources();
			throw VideoOutRenderException("libplacebo failed to upload the source frame.");
		}

		uploaded_plane.flipped = frame.flipped;
		uploaded_plane.address_mode = PL_TEX_ADDRESS_CLAMP;

		plane_state.texture = uploaded_plane.texture;
		plane_state.components = uploaded_plane.components;
		plane_state.flipped = uploaded_plane.flipped;
		plane_state.shift_x = uploaded_plane.shift_x;
		plane_state.shift_y = uploaded_plane.shift_y;
		for (int component = 0; component < 4; ++component)
			plane_state.component_mapping[static_cast<size_t>(component)] =
				uploaded_plane.component_mapping[component];
	}

	image_width = frame.width;
	image_height = frame.height;
	image_plane_count = next_plane_count;
	image_output_mode = frame.output_mode;
	image_format_info = frame.format_info;
	image_color = frame.color;
	image_chroma_location = frame.chroma_location;
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

	SourceFrame frame_description;
	frame_description.output_mode = image_output_mode;
	frame_description.format_info = image_format_info;
	frame_description.color = image_color;
	frame_description.chroma_location = image_chroma_location;

	struct pl_frame image = {};
	image.num_planes = image_plane_count;
	for (int i = 0; i < image_plane_count; ++i) {
		auto const& plane_state = image_planes[static_cast<size_t>(i)];
		auto& plane = image.planes[static_cast<size_t>(i)];
		plane.texture = plane_state.texture;
		plane.address_mode = PL_TEX_ADDRESS_CLAMP;
		plane.flipped = plane_state.flipped;
		plane.components = plane_state.components;
		plane.shift_x = plane_state.shift_x;
		plane.shift_y = plane_state.shift_y;
		for (int component = 0; component < 4; ++component)
			plane.component_mapping[component] = plane_state.component_mapping[static_cast<size_t>(component)];
	}
	image.repr = BuildPlaceboSourceFrameRepr(frame_description);
	image.color = BuildPlaceboSourceFrameColorSpace(frame_description);
	image.crop = FullRect(image_width, image_height);
	image.rotation = PL_ROTATION_0;
	if (api->frame_set_chroma_location && PlaceboSourceFrameNeedsExplicitChromaLocation(frame_description))
		api->frame_set_chroma_location(&image, ResolvePlaceboChromaLocation(frame_description));

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
	target.repr = BuildPlaceboRenderTargetRepr();
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
