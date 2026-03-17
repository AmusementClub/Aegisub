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

#include "video_renderer_factory.h"

#include "video_renderer_opengl.h"
#include "options.h"

#ifdef WITH_LIBPLACEBO
#include "video_renderer_placebo_gl.h"
#endif

#include <libaegisub/exception.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

namespace {
constexpr char const *kPlaceboLogTag = "video/out/placebo";
}

VideoRendererCreateResult CreateConfiguredVideoRenderer() {
	return CreateVideoRendererForOption(OPT_GET("Video/Renderer/Backend")->GetString());
}

VideoRendererCreateResult CreateVideoRendererForOption(std::string_view option_value) {
	VideoRendererCreateResult result;
	result.requested = ParseVideoRendererBackend(option_value);
	result.actual = VideoRendererBackend::OpenGL;

	if (result.requested == VideoRendererBackend::PlaceboOpenGL) {
#ifdef WITH_LIBPLACEBO
		try {
			result.renderer = agi::make_unique<PlaceboRendererGL>();
			result.actual = VideoRendererBackend::PlaceboOpenGL;
			return result;
		}
		catch (agi::Exception const& err) {
			result.fell_back = true;
			result.fallback_reason = err.GetMessage();
			LOG_W(kPlaceboLogTag) << "Failed to activate runtime-loaded libplacebo backend; falling back to OpenGLVideoRenderer: "
				<< result.fallback_reason;
		}
#else
		result.fell_back = true;
		result.fallback_reason = "libplacebo support was not compiled into this build.";
		LOG_W(kPlaceboLogTag) << "libplacebo backend was requested, but support is not compiled in; falling back to OpenGLVideoRenderer.";
#endif
	}

	result.renderer = agi::make_unique<OpenGLVideoRenderer>();
	return result;
}
