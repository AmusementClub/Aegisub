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

struct VideoRenderOutputLayout {
	int source_width = 0;
	int source_height = 0;
	int output_width = 0;
	int output_height = 0;
	int rotation = 0;
	bool display_vflip = false;
};

struct VideoRenderPoint {
	float x = 0.0f;
	float y = 0.0f;
};

struct VideoRenderQuad {
	VideoRenderPoint p0;
	VideoRenderPoint p1;
	VideoRenderPoint p2;
	VideoRenderPoint p3;
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

inline VideoRenderOutputLayout BuildVideoRenderOutputLayout(
	int source_width,
	int source_height,
	int rotation_degrees = 0) {
	auto const rotation = NormalizeSourceFrameRotationDegrees(rotation_degrees);
	bool const swap_axes = rotation == 90 || rotation == 270;
	return {
		source_width,
		source_height,
		swap_axes ? source_height : source_width,
		swap_axes ? source_width : source_height,
		rotation,
		false
	};
}

inline VideoRenderOutputLayout BuildVideoRenderOutputLayout(
	int source_width,
	int source_height,
	SourceFrameGeometry const& geometry) {
	auto layout = BuildVideoRenderOutputLayout(source_width, source_height, geometry.rotation);
	layout.display_vflip = geometry.display_vflip;
	return layout;
}

inline VideoRenderOutputLayout BuildVideoRenderOutputLayout(
	VideoRenderCanvasLayout const& canvas,
	SourceFrameGeometry const& geometry) {
	return BuildVideoRenderOutputLayout(canvas.canvas_width, canvas.canvas_height, geometry);
}

inline VideoRenderPoint TransformVideoRenderPoint(
	VideoRenderOutputLayout const& layout,
	float x,
	float y) {
	switch (layout.rotation) {
		case 90:
		{
			float original_x = x;
			x = static_cast<float>(layout.source_height) - y;
			y = original_x;
			break;
		}
		case 180:
			x = static_cast<float>(layout.source_width) - x;
			y = static_cast<float>(layout.source_height) - y;
			break;
		case 270:
		{
			float original_x = x;
			x = y;
			y = static_cast<float>(layout.source_width) - original_x;
			break;
		}
		default:
			break;
	}

	if (layout.display_vflip)
		y = static_cast<float>(layout.output_height) - y;

	return { x, y };
}

inline VideoRenderQuad TransformVideoRenderQuad(
	VideoRenderOutputLayout const& layout,
	float x1,
	float y1,
	float x2,
	float y2) {
	return {
		TransformVideoRenderPoint(layout, x1, y1),
		TransformVideoRenderPoint(layout, x2, y1),
		TransformVideoRenderPoint(layout, x2, y2),
		TransformVideoRenderPoint(layout, x1, y2)
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

	int const overlay_x0 = overlay.target_x;
	int const overlay_y0 = overlay.target_y;
	int const overlay_x1 = overlay.target_x + overlay.width;
	int const overlay_y1 = overlay.target_y + overlay.height;
	int const clip_x0 = std::max(overlay_x0, visible.x);
	int const clip_y0 = std::max(overlay_y0, visible.y);
	int const clip_x1 = std::min(overlay_x1, visible.x + visible.width);
	int const clip_y1 = std::min(overlay_y1, visible.y + visible.height);

	if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
		auto hidden = overlay;
		hidden.has_visible_content = false;
		hidden.dirty_rects = nullptr;
		hidden.dirty_rect_count = 0;
		return hidden;
	}

	auto adjusted = overlay;
	adjusted.canvas_width = visible.width;
	adjusted.canvas_height = visible.height;

	bool const fully_inside_visible =
		clip_x0 == overlay_x0 &&
		clip_y0 == overlay_y0 &&
		clip_x1 == overlay_x1 &&
		clip_y1 == overlay_y1;
	if (fully_inside_visible) {
		adjusted.target_x -= visible.x;
		adjusted.target_y -= visible.y;
		return adjusted;
	}

	// Direct-renderable overlays currently use a single BGRA plane, so we can
	// cheaply crop by rebasing the view without allocating a temporary surface.
	if (adjusted.pixel_format != SubtitleOverlayPixelFormat::Bgra8
		|| adjusted.plane_count <= 0
		|| !adjusted.planes[0].data) {
		adjusted.has_visible_content = false;
		adjusted.dirty_rects = nullptr;
		adjusted.dirty_rect_count = 0;
		return adjusted;
	}

	adjusted.target_x = clip_x0 - visible.x;
	adjusted.target_y = clip_y0 - visible.y;
	adjusted.width = clip_x1 - clip_x0;
	adjusted.height = clip_y1 - clip_y0;
	adjusted.planes[0].data +=
		static_cast<ptrdiff_t>(clip_y0 - overlay_y0) * adjusted.planes[0].stride +
		static_cast<ptrdiff_t>(clip_x0 - overlay_x0) * 4;
	adjusted.planes[0].width = adjusted.width;
	adjusted.planes[0].height = adjusted.height;
	if (adjusted.dirty_rect_count > 0 && adjusted.dirty_rects) {
		adjusted.dirty_rects = nullptr;
		adjusted.dirty_rect_count = 0;
		adjusted.force_full_upload = true;
	}
	return adjusted;
}
