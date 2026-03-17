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

#include "video_renderer_placebo_runtime.h"
#include "video_renderer_error.h"

#include <libaegisub/log.h>

namespace {
constexpr char const *kDeferredActivationMessage =
	"libplacebo runtime loading is wired up, but the positive renderer path remains disabled until DLL-backed validation is available.";
}

PlaceboRendererGL::PlaceboRendererGL() {
	placebo::runtime::EnsureLoaded();
	LOG_W("video/out/placebo") << kDeferredActivationMessage;
	throw VideoOutInitException(kDeferredActivationMessage);
}

void PlaceboRendererGL::Reset() {
}

void PlaceboRendererGL::UploadFrame(SourceFrame const&) {
}

void PlaceboRendererGL::UploadOverlay(SubtitleOverlay const*) {
}

void PlaceboRendererGL::Render(RenderViewport const&, int, int) {
}
