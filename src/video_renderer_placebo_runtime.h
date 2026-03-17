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
	decltype(&pl_upload_plane) upload_plane = nullptr;
	decltype(&pl_tex_destroy) tex_destroy = nullptr;
	decltype(&pl_render_image) render_image = nullptr;
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
std::string GetLoadedVersion();
Api const& GetApi();

} }
