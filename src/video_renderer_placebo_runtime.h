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

#pragma once

#ifndef WITH_LIBPLACEBO
#error "video_renderer_placebo_runtime.h requires WITH_LIBPLACEBO"
#endif

#include <libplacebo/log.h>
#include <libplacebo/opengl.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>

#include <cstdint>
#include <string>

namespace placebo { namespace runtime {

struct Api {
	using LogCreate = pl_log (*)(int api_ver, const struct pl_log_params *params);

	LogCreate log_create = nullptr;
	decltype(&pl_log_destroy) log_destroy = nullptr;
	decltype(&pl_version) version = nullptr;
	decltype(&pl_fix_ver) fix_ver = nullptr;
	decltype(&pl_opengl_create) opengl_create = nullptr;
	decltype(&pl_opengl_destroy) opengl_destroy = nullptr;
	decltype(&pl_renderer_create) renderer_create = nullptr;
	decltype(&pl_renderer_destroy) renderer_destroy = nullptr;
	decltype(&pl_renderer_flush_cache) renderer_flush_cache = nullptr;
	decltype(&pl_opengl_wrap) opengl_wrap = nullptr;
	decltype(&pl_plane_data_from_mask) plane_data_from_mask = nullptr;
	decltype(&pl_plane_data_align) plane_data_align = nullptr;
	decltype(&pl_upload_plane) upload_plane = nullptr;
	decltype(&pl_tex_destroy) tex_destroy = nullptr;
	decltype(&pl_frame_set_chroma_location) frame_set_chroma_location = nullptr;
	decltype(&pl_render_image) render_image = nullptr;
	using HdrRescale = float (*)(enum pl_hdr_scaling from, enum pl_hdr_scaling to, float x);
	HdrRescale hdr_rescale = nullptr;
	using HdrMetadataFromDoviRpu = void (*)(struct pl_hdr_metadata *out, const uint8_t *buf, size_t size);
	HdrMetadataFromDoviRpu hdr_metadata_from_dovi_rpu = nullptr;
	using MapAVFrame = bool (*)(pl_gpu gpu, struct pl_frame *out_frame, pl_tex tex[4], const void *avframe);
	using UnmapAVFrame = void (*)(pl_gpu gpu, struct pl_frame *frame);
	MapAVFrame map_avframe = nullptr;
	UnmapAVFrame unmap_avframe = nullptr;

	// Optional helper used when HDR sources are tone-mapped to the SDR preview target.
	using ColorSpaceIsHDR = bool (*)(const struct pl_color_space *csp);
	ColorSpaceIsHDR color_space_is_hdr = nullptr;
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
std::string GetLoadedVersion();
uint32_t GetLoadedFixVersion() noexcept;
Api const& GetApi();

} }
