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

#include "render_types.h"
#include "source_frame.h"

#include <cstddef>
#include <vector>

struct SubtitleOverlay;

class IVideoRenderer {
public:
	virtual ~IVideoRenderer() = default;

	virtual bool SupportsDirectOverlay() const noexcept { return true; }
	virtual char const* GetDebugName() const noexcept { return "Unknown"; }
	virtual size_t EstimateTextureBytes() const noexcept { return 0; }
	virtual std::vector<SourceFrameOutputMode> GetPreferredSourceModes() const {
		return { SourceFrameOutputMode::Bgra8 };
	}
	virtual void Reset() = 0;
	virtual void UploadFrame(SourceFrame const& frame) = 0;
	virtual void UploadOverlay(SubtitleOverlay const* overlay) = 0;
	virtual void Render(RenderViewport const& viewport, int canvas_width, int canvas_height) = 0;
};
