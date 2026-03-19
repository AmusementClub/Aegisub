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
