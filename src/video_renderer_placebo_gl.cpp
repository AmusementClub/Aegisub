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

#include "video_render_opengl_proc_loader.h"
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
		pixels += static_cast<ptrdiff_t>(plane.height - 1) * stride;

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
	image_geometry = {};
	has_frame = false;
}

void PlaceboRendererGL::DestroyTargetResources() noexcept {
	if (api && opengl && target_texture)
		api->tex_destroy(opengl->gpu, &target_texture);
	target_texture = nullptr;
	target_width = 0;
	target_height = 0;
	target_framebuffer = 0;
	target_texture_estimated_bytes = 0;
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
	GLint framebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
	auto const framebuffer_id = static_cast<unsigned int>(framebuffer);
	if (target_texture
		&& target_width == canvas_width
		&& target_height == canvas_height
		&& target_framebuffer == framebuffer_id)
		return;

	DestroyTargetResources();

	struct pl_opengl_wrap_params params = {};
	params.framebuffer = framebuffer_id;
	params.width = canvas_width;
	params.height = canvas_height;
	target_texture = api->opengl_wrap(opengl->gpu, &params);
	if (!target_texture)
		throw VideoOutInitException("Failed to wrap the current OpenGL framebuffer for libplacebo rendering.");

	target_width = canvas_width;
	target_height = canvas_height;
	target_framebuffer = framebuffer_id;
	target_texture_estimated_bytes = static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height) * 4;
}

void PlaceboRendererGL::UploadFrame(SourceFrame const& frame) {
	if (!frame.IsValid()) {
		DestroyImageResources();
		return;
	}

	EnsureInitialized();

	SourceFrame const* upload_frame = &frame;
	SourceFrame transformed_frame;
	std::array<SourceFramePlaneView, 4> transformed_planes = { };
	if (SourceFrameHasUnbakedDisplayTransform(frame) && frame.geometry.display_vflip) {
		// Preserve native chroma siting by expressing display_vflip as a
		// negative-stride source view before upload instead of flipping the
		// reconstructed RGB target after sampling.
		transformed_frame = frame;
		transformed_planes = frame.planes;
		transformed_frame.geometry.display_vflip = false;
		for (int i = 0; i < frame.plane_count; ++i) {
			auto& plane = transformed_planes[static_cast<size_t>(i)];
			if (!plane.data || plane.height <= 0 || plane.stride == 0)
				continue;
			plane.data += static_cast<ptrdiff_t>(plane.height - 1) * plane.stride;
			plane.stride = -plane.stride;
		}
		transformed_frame.planes = transformed_planes;
		upload_frame = &transformed_frame;
	}

	int const next_plane_count = upload_frame->output_mode == SourceFrameOutputMode::Native ? upload_frame->plane_count : 1;
	for (int i = next_plane_count; i < image_plane_count; ++i) {
		auto& plane = image_planes[static_cast<size_t>(i)];
		if (plane.texture)
			api->tex_destroy(opengl->gpu, &plane.texture);
		plane = {};
	}

	for (int i = 0; i < next_plane_count; ++i) {
		struct pl_plane_data data = {};
		bool built = upload_frame->output_mode == SourceFrameOutputMode::Bgra8
			? BuildBgra8PlaneData(*api, *upload_frame, data)
			: BuildPlaceboNativePlaneData(*upload_frame, i, data);
		if (!built) {
			DestroyImageResources();
			throw VideoOutRenderException("libplacebo could not describe the source frame for upload.");
		}
		if (upload_frame->output_mode == SourceFrameOutputMode::Native && api->plane_data_align) {
			struct pl_bit_encoding ignored_bits = {};
			api->plane_data_align(&data, &ignored_bits);
		}

		struct pl_plane uploaded_plane = {};
		auto& plane_state = image_planes[static_cast<size_t>(i)];
		if (!api->upload_plane(opengl->gpu, &uploaded_plane, &plane_state.texture, &data)) {
			DestroyImageResources();
			throw VideoOutRenderException("libplacebo failed to upload the source frame.");
		}

		uploaded_plane.flipped =
			(upload_frame->planes[static_cast<size_t>(i)].stride < 0) ^ upload_frame->flipped;
		uploaded_plane.address_mode = PL_TEX_ADDRESS_CLAMP;

		plane_state.texture = uploaded_plane.texture;
		plane_state.components = uploaded_plane.components;
		plane_state.flipped = uploaded_plane.flipped;
		plane_state.shift_x = uploaded_plane.shift_x;
		plane_state.shift_y = uploaded_plane.shift_y;
		plane_state.estimated_bytes = static_cast<size_t>(data.row_stride) * static_cast<size_t>(data.height);
		for (int component = 0; component < 4; ++component)
			plane_state.component_mapping[static_cast<size_t>(component)] =
				uploaded_plane.component_mapping[component];
	}

	image_width = upload_frame->width;
	image_height = upload_frame->height;
	image_plane_count = next_plane_count;
	image_output_mode = upload_frame->output_mode;
	image_format_info = upload_frame->format_info;
	image_color = upload_frame->color;
	image_chroma_location = upload_frame->chroma_location;
	image_geometry = upload_frame->geometry;
	has_frame = true;
}

size_t PlaceboRendererGL::EstimateTextureBytes() const noexcept {
	size_t total_bytes = target_texture_estimated_bytes;
	for (auto const& plane : image_planes)
		total_bytes += plane.estimated_bytes;
	return total_bytes;
}

void PlaceboRendererGL::UploadOverlay(SubtitleOverlay const*) {
}

void PlaceboRendererGL::RestoreCompatibilityState() noexcept {
	if (functions) {
		if (functions->UseProgram)
			functions->UseProgram(0);
		if (functions->BindFramebuffer)
			functions->BindFramebuffer(GL_FRAMEBUFFER, target_framebuffer);
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
	frame_description.width = image_width;
	frame_description.height = image_height;
	frame_description.geometry = image_geometry;

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
	image.crop = BuildPlaceboSourceFrameCropRect(frame_description);
	image.rotation = BuildPlaceboSourceFrameRotation(frame_description);
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
	target.crop = BuildPlaceboRenderTargetCropRect(viewport, canvas_width, canvas_height);
	target.rotation = PL_ROTATION_0;

	struct pl_render_params params = {};
	if (!api->render_image(renderer, &image, &target, &params)) {
		RestoreCompatibilityState();
		throw VideoOutRenderException("libplacebo failed to render the video frame.");
	}

	RestoreCompatibilityState();
}
