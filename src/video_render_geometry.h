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

#include "source_frame.h"
#include "subtitle_overlay.h"

struct VideoRenderCanvasLayout {
	int canvas_width = 0;
	int canvas_height = 0;
	int offset_x = 0;
	int offset_y = 0;
};

inline VideoRenderCanvasLayout BuildVideoRenderCanvasLayout(SourceFrame const& frame) {
	auto const visible = GetSourceFrameVisibleRect(frame);
	return {
		visible.width,
		visible.height,
		-visible.x,
		-visible.y
	};
}

inline SubtitleOverlay AdjustSubtitleOverlayForSourceGeometry(
	SubtitleOverlay const& overlay,
	SourceFrameGeometry const& geometry) {
	if (overlay.coordinate_space != SubtitleOverlayCoordinateSpace::SourceStorage)
		return overlay;

	auto const visible = GetSourceFrameVisibleRect(geometry, overlay.canvas_width, overlay.canvas_height);
	if (!visible.IsValid())
		return overlay;

	auto adjusted = overlay;
	adjusted.canvas_width = visible.width;
	adjusted.canvas_height = visible.height;
	adjusted.target_x -= visible.x;
	adjusted.target_y -= visible.y;
	return adjusted;
}
