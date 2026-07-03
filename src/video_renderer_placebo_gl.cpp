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
#include "legacy_gl_draw.h"
#include "subtitle_overlay.h"
#include "video_renderer_error.h"
#include "video_renderer_placebo_runtime.h"
#include "video_renderer_placebo_source_frame.h"

#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/utils/dolbyvision.h>

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

void CopyDolbyVisionComponent(pl_dovi_metadata::pl_reshape_data& dst, SourceFrameDolbyVisionReshapeComponent const& src) {
	dst.num_pivots = src.num_pivots;
	for (int i = 0; i < 9; ++i)
		dst.pivots[i] = src.pivots[static_cast<size_t>(i)];
	for (int i = 0; i < 8; ++i) {
		dst.method[i] = src.method[static_cast<size_t>(i)];
		for (int k = 0; k < 3; ++k)
			dst.poly_coeffs[i][k] = src.poly_coeffs[static_cast<size_t>(i)][static_cast<size_t>(k)];
		dst.mmr_order[i] = src.mmr_order[static_cast<size_t>(i)];
		dst.mmr_constant[i] = src.mmr_constant[static_cast<size_t>(i)];
		for (int j = 0; j < 3; ++j) {
			for (int k = 0; k < 7; ++k)
				dst.mmr_coeffs[i][j][k] = src.mmr_coeffs[static_cast<size_t>(i)][static_cast<size_t>(j)][static_cast<size_t>(k)];
		}
	}
}

void FillDolbyVisionMetadata(pl_dovi_metadata& dst, SourceFrameDolbyVisionMetadata const& src) {
	for (int i = 0; i < 3; ++i)
		dst.nonlinear_offset[i] = src.nonlinear_offset[static_cast<size_t>(i)];
	auto *nonlinear = &dst.nonlinear.m[0][0];
	auto *linear = &dst.linear.m[0][0];
	for (int i = 0; i < 9; ++i) {
		nonlinear[i] = src.nonlinear[static_cast<size_t>(i)];
		linear[i] = src.linear[static_cast<size_t>(i)];
	}
	for (int c = 0; c < 3; ++c)
		CopyDolbyVisionComponent(dst.comp[c], src.comp[static_cast<size_t>(c)]);
}

void ThrowGlRenderError(char const* operation, GLenum err) {
	if (agi::log::log)
		LOG_E(kPlaceboLogTag) << operation << " failed with error code " << err;
	throw VideoOutRenderException(operation, err);
}

void CheckGlRenderError(char const* operation) {
	if (GLenum err = glGetError())
		ThrowGlRenderError(operation, err);
}

void ApplyDolbyVisionMetadata(
	placebo::runtime::Api const& api,
	struct pl_frame& image,
	SourceFrameDolbyVisionMetadata const& dovi,
	struct pl_dovi_metadata const *metadata) {
	if (!dovi.valid || !metadata)
		return;

	image.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
	image.repr.dovi = metadata;
	image.color.primaries = PL_COLOR_PRIM_BT_2020;
	image.color.transfer = PL_COLOR_TRC_PQ;
	if (api.hdr_rescale) {
		image.color.hdr.min_luma = api.hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, dovi.source_min_pq);
		image.color.hdr.max_luma = api.hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, dovi.source_max_pq);
	}
	image.color.hdr.max_pq_y = dovi.max_pq_y;
	image.color.hdr.avg_pq_y = dovi.avg_pq_y;
	if (api.hdr_metadata_from_dovi_rpu && !dovi.rpu.empty())
		api.hdr_metadata_from_dovi_rpu(&image.color.hdr, dovi.rpu.data(), dovi.rpu.size());
}

}

struct PlaceboRendererGL::Functions {
	PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
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
		functions->ActiveTexture = LoadOptionalProc<PFNGLACTIVETEXTUREPROC>("glActiveTexture");
		functions->BindBuffer = LoadOptionalProc<PFNGLBINDBUFFERPROC>("glBindBuffer");
		functions->UseProgram = LoadOptionalProc<PFNGLUSEPROGRAMPROC>("glUseProgram");
		functions->BindFramebuffer = LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT");
		functions->BindVertexArray = LoadOptionalProc<PFNGLBINDVERTEXARRAYPROC>("glBindVertexArray");
		functions->DeleteFramebuffers = LoadOptionalProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
		functions->GenFramebuffers = LoadOptionalProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT");
		functions->FramebufferTexture2D = LoadOptionalProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT");
		functions->CheckFramebufferStatus = LoadOptionalProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");

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

void PlaceboRendererGL::DestroyMappedAVFrame() noexcept {
	if (api && opengl && mapped_avframe && api->unmap_avframe)
		api->unmap_avframe(opengl->gpu, mapped_avframe.get());
	mapped_avframe.reset();
}

void PlaceboRendererGL::DestroyPlaneResources() noexcept {
	for (auto& plane : image_planes) {
		if (api && opengl && plane.texture)
			api->tex_destroy(opengl->gpu, &plane.texture);
		plane = {};
	}
}

void PlaceboRendererGL::DestroyAVFrameTextures() noexcept {
	for (auto& texture : image_avframe_textures) {
		if (api && opengl && texture)
			api->tex_destroy(opengl->gpu, &texture);
		texture = nullptr;
	}
	image_avframe_texture_estimated_bytes = 0;
}

void PlaceboRendererGL::DestroyImageResources() noexcept {
	DestroyMappedAVFrame();
	DestroyPlaneResources();
	DestroyAVFrameTextures();
	image_width = 0;
	image_height = 0;
	image_plane_count = 0;
	image_output_mode = SourceFrameOutputMode::Bgra8;
	image_format_info = {};
	image_color = {};
	image_dolby_vision = {};
	image_chroma_location = SourceFrameChromaLocation::Unknown;
	image_geometry = {};
	image_placebo_dovi_metadata.reset();
	has_frame = false;
}

void PlaceboRendererGL::DestroyTargetResources() noexcept {
	if (api && opengl && target_texture)
		api->tex_destroy(opengl->gpu, &target_texture);
	target_texture = nullptr;
	if (functions && functions->DeleteFramebuffers && target_render_framebuffer) {
		auto framebuffer = static_cast<GLuint>(target_render_framebuffer);
		functions->DeleteFramebuffers(1, &framebuffer);
	}
	target_render_framebuffer = 0;
	if (target_gl_texture) {
		auto texture = static_cast<GLuint>(target_gl_texture);
		glDeleteTextures(1, &texture);
	}
	target_gl_texture = 0;
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
		&& target_height == canvas_height) {
		target_framebuffer = framebuffer_id;
		return;
	}

	DestroyTargetResources();
	target_framebuffer = framebuffer_id;

	// Render libplacebo into our own RGBA texture, then present it using the
	// same fixed-function textured quad path as the legacy OpenGL renderer.
	if (!functions
		|| !functions->BindFramebuffer
		|| !functions->DeleteFramebuffers
		|| !functions->GenFramebuffers
		|| !functions->FramebufferTexture2D
		|| !functions->CheckFramebufferStatus)
		throw VideoOutInitException("OpenGL framebuffer objects are required for libplacebo offscreen rendering.");

	GLuint texture = 0;
	GLuint framebuffer_object = 0;
	auto cleanup = agi::make_scope_exit([&] {
		if (framebuffer_object)
			functions->DeleteFramebuffers(1, &framebuffer_object);
		if (texture)
			glDeleteTextures(1, &texture);
	});

	GLint previous_framebuffer = 0;
	GLint previous_draw_buffer = GL_BACK;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glGetIntegerv(GL_DRAW_BUFFER, &previous_draw_buffer);
	auto restore_framebuffer = agi::make_scope_exit([&] {
		functions->BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
		glDrawBuffer(static_cast<GLenum>(previous_draw_buffer));
	});

	glGenTextures(1, &texture);
	CheckGlRenderError("glGenTextures");
	glBindTexture(GL_TEXTURE_2D, texture);
	CheckGlRenderError("glBindTexture");
	auto restore_texture = agi::make_scope_exit([] { glBindTexture(GL_TEXTURE_2D, 0); });
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	CheckGlRenderError("glTexParameteri(GL_TEXTURE_MIN_FILTER)");
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	CheckGlRenderError("glTexParameteri(GL_TEXTURE_MAG_FILTER)");
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	CheckGlRenderError("glTexParameteri(GL_TEXTURE_WRAP_S)");
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	CheckGlRenderError("glTexParameteri(GL_TEXTURE_WRAP_T)");
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
	CheckGlRenderError("glTexImage2D");

	functions->GenFramebuffers(1, &framebuffer_object);
	CheckGlRenderError("glGenFramebuffers");
	functions->BindFramebuffer(GL_FRAMEBUFFER, framebuffer_object);
	CheckGlRenderError("glBindFramebuffer");
	functions->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	CheckGlRenderError("glFramebufferTexture2D");
	if (functions->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		throw VideoOutInitException("Failed to create libplacebo offscreen render framebuffer.");

	struct pl_opengl_wrap_params params = {};
	params.texture = texture;
	params.framebuffer = framebuffer_object;
	params.width = canvas_width;
	params.height = canvas_height;
	params.target = GL_TEXTURE_2D;
	params.iformat = GL_RGBA8;
	target_texture = api->opengl_wrap(opengl->gpu, &params);
	if (!target_texture)
		throw VideoOutInitException("Failed to wrap the offscreen OpenGL texture for libplacebo rendering.");

	target_gl_texture = texture;
	target_render_framebuffer = framebuffer_object;
	target_width = canvas_width;
	target_height = canvas_height;
	target_texture_estimated_bytes = static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height) * 4;
	texture = 0;
	framebuffer_object = 0;
}

void PlaceboRendererGL::UploadFrame(SourceFrame const& frame) {
	if (!frame.IsValid()) {
		DestroyImageResources();
		return;
	}

	EnsureInitialized();
	DestroyMappedAVFrame();
	legacy_gl::ResetPixelStoreState();
	auto restore_pixel_store = agi::make_scope_exit([] { legacy_gl::ResetPixelStoreState(); });

	if (frame.output_mode == SourceFrameOutputMode::Native
		&& frame.native_payload_kind == SourceFrameNativePayloadKind::FFmpegAVFrame
		&& frame.native_payload
		&& api->map_avframe
		&& api->unmap_avframe) {
		auto mapped = std::make_unique<pl_frame>();
		bool mapped_ok = api->map_avframe(opengl->gpu, mapped.get(), image_avframe_textures.data(), frame.native_payload);
		if (mapped_ok) {
			DestroyPlaneResources();
			mapped_avframe = std::move(mapped);
			image_avframe_texture_estimated_bytes = 0;
			for (auto texture : image_avframe_textures) {
				if (texture)
					image_avframe_texture_estimated_bytes += static_cast<size_t>(texture->params.w) * static_cast<size_t>(texture->params.h) * 4;
			}
			image_width = frame.width;
			image_height = frame.height;
			image_plane_count = mapped_avframe->num_planes;
			image_output_mode = frame.output_mode;
			image_format_info = frame.format_info;
			image_color = frame.color;
			image_dolby_vision = frame.dolby_vision;
			if (image_dolby_vision.valid) {
				image_placebo_dovi_metadata = std::make_unique<pl_dovi_metadata>();
				FillDolbyVisionMetadata(*image_placebo_dovi_metadata, image_dolby_vision);
			}
			else {
				image_placebo_dovi_metadata.reset();
			}
			image_chroma_location = frame.chroma_location;
			image_geometry = frame.geometry;
			has_frame = true;
			return;
		}
	}
	DestroyAVFrameTextures();

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
	image_dolby_vision = upload_frame->dolby_vision;
	if (image_dolby_vision.valid) {
		image_placebo_dovi_metadata = std::make_unique<pl_dovi_metadata>();
		FillDolbyVisionMetadata(*image_placebo_dovi_metadata, image_dolby_vision);
	}
	else {
		image_placebo_dovi_metadata.reset();
	}
	image_chroma_location = upload_frame->chroma_location;
	image_geometry = upload_frame->geometry;
	has_frame = true;
}

size_t PlaceboRendererGL::EstimateTextureBytes() const noexcept {
	size_t total_bytes = target_texture_estimated_bytes;
	for (auto const& plane : image_planes)
		total_bytes += plane.estimated_bytes;
	total_bytes += image_avframe_texture_estimated_bytes;
	return total_bytes;
}

void PlaceboRendererGL::UploadOverlay(SubtitleOverlay const*) {
}

void PlaceboRendererGL::Render(RenderViewport const& viewport, int canvas_width, int canvas_height) {
	if (!has_frame || viewport.width <= 0 || viewport.height <= 0)
		return;

	EnsureInitialized();
	RecreateTargetTexture(canvas_width, canvas_height);

	GLint output_draw_buffer = GL_BACK;
	glGetIntegerv(GL_DRAW_BUFFER, &output_draw_buffer);
	auto restore_output_framebuffer = agi::make_scope_exit([&] {
		if (functions && functions->BindFramebuffer) {
			functions->BindFramebuffer(GL_FRAMEBUFFER, target_framebuffer);
			glDrawBuffer(static_cast<GLenum>(output_draw_buffer));
		}
		legacy_gl::ResetCompatibilityState();
	});

	if (functions && functions->BindFramebuffer) {
		functions->BindFramebuffer(GL_FRAMEBUFFER, target_render_framebuffer);
		CheckGlRenderError("glBindFramebuffer");
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		CheckGlRenderError("glDrawBuffer");
	}
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
	frame_description.dolby_vision = image_dolby_vision;
	frame_description.chroma_location = image_chroma_location;
	frame_description.width = image_width;
	frame_description.height = image_height;
	frame_description.geometry = image_geometry;

	struct pl_frame image = mapped_avframe ? *mapped_avframe : pl_frame{};
	if (!mapped_avframe) {
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
		if (api->frame_set_chroma_location && PlaceboSourceFrameNeedsExplicitChromaLocation(frame_description))
			api->frame_set_chroma_location(&image, ResolvePlaceboChromaLocation(frame_description));
	}
	ApplyDolbyVisionMetadata(*api, image, image_dolby_vision, image_placebo_dovi_metadata.get());
	image.crop = BuildPlaceboSourceFrameCropRect(frame_description);
	image.rotation = BuildPlaceboSourceFrameRotation(frame_description);

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

	// HDR video is always rendered into an SDR preview target. Subtitles are
	// composited after this pass, so they match the tone-mapped image instead
	// of an HDR signal that Aegisub cannot preserve end-to-end.
	bool source_is_hdr = false;
	if (api->color_space_is_hdr)
		source_is_hdr = api->color_space_is_hdr(&image.color);
	else
		source_is_hdr = (image.color.transfer == PL_COLOR_TRC_PQ || image.color.transfer == PL_COLOR_TRC_HLG);

	struct pl_render_params params = {};

	if (source_is_hdr) {
		target.color = BuildPlaceboSDRRenderTargetColorSpace();
	}

	if (!api->render_image(renderer, &image, &target, &params))
		throw VideoOutRenderException("libplacebo failed to render the video frame.");

	if (functions && functions->BindFramebuffer) {
		functions->BindFramebuffer(GL_FRAMEBUFFER, target_framebuffer);
		CheckGlRenderError("glBindFramebuffer");
		glDrawBuffer(static_cast<GLenum>(output_draw_buffer));
		CheckGlRenderError("glDrawBuffer");
	}
	legacy_gl::ResetCompatibilityState();

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glViewport(0, 0, canvas_width, canvas_height);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	legacy_gl::DrawTexturedQuad(target_gl_texture, canvas_width, canvas_height);
	if (GLenum err = glGetError())
		ThrowGlRenderError("legacy_gl::DrawTexturedQuad", err);
}
