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

#include "ivideo_renderer.h"
#include "video_renderer_backend.h"

#include <string>
#include <string_view>

struct VideoRendererCreateResult {
	std::unique_ptr<IVideoRenderer> renderer;
	VideoRendererBackend requested = VideoRendererBackend::OpenGL;
	VideoRendererBackend actual = VideoRendererBackend::OpenGL;
	bool fell_back = false;
	std::string fallback_reason;
};

VideoRendererCreateResult CreateConfiguredVideoRenderer();
VideoRendererCreateResult CreateVideoRendererForOption(std::string_view option_value);
