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

#include <algorithm>
#include <cmath>

#include "vector2d.h"

struct VideoDisplayViewportLayout {
	int viewport_left = 0;
	int viewport_width = 0;
	int viewport_bottom = 0;
	int viewport_top = 0;
	int viewport_height = 0;
};

enum VideoDisplayScrollAction {
	SCALE_VIDEO = 0,
	SCALE_VIDEO_REV = 1,
	ZOOM_VIDEO = 2,
	ZOOM_VIDEO_REV = 3,
	PAN_VIDEO = 4,
	PAN_VIDEO_SWAP = 5,
	NOTHING = 6
};

struct VideoDisplayContentTransform {
	double zoom = 1.0;
	double pan_x = 0.0;
	double pan_y = 0.0;
};

inline VideoDisplayViewportLayout BuildVideoDisplayViewportLayout(
	int canvas_width,
	int canvas_height,
	int video_width,
	int video_height,
	bool free_size,
	double target_display_aspect_ratio = 0.0) {
	VideoDisplayViewportLayout layout;
	layout.viewport_left = 0;
	layout.viewport_bottom = canvas_height - video_height;
	layout.viewport_top = 0;
	layout.viewport_width = video_width;
	layout.viewport_height = video_height;

	if (!free_size || video_width <= 0 || video_height <= 0)
		return layout;

	double video_aspect_ratio = target_display_aspect_ratio > 0.0
		? target_display_aspect_ratio
		: static_cast<double>(video_width) / video_height;
	double display_aspect_ratio = static_cast<double>(layout.viewport_width) / layout.viewport_height;

	// Window is wider than video, blackbox left/right.
	if (display_aspect_ratio - video_aspect_ratio > 0.01) {
		int delta = layout.viewport_width - video_aspect_ratio * layout.viewport_height;
		layout.viewport_left = delta / 2;
		layout.viewport_width -= delta;
	}
	// Video is wider than window, blackbox top/bottom.
	else if (video_aspect_ratio - display_aspect_ratio > 0.01) {
		int delta = layout.viewport_height - layout.viewport_width / video_aspect_ratio;
		layout.viewport_top = delta / 2;
		layout.viewport_bottom = delta / 2;
		layout.viewport_height -= delta;
	}

	return layout;
}

inline double ClampVideoDisplayNormalizedPan(double pan, int content_size, int viewport_size) {
	if (viewport_size <= 0)
		return 0.0;

	double const max_pan =
		(0.5 * static_cast<double>(content_size) + 0.4 * static_cast<double>(viewport_size))
		/ static_cast<double>(viewport_size);
	return std::clamp(pan, -max_pan, max_pan);
}

inline double ClampVideoDisplayContentZoom(double zoom) {
	return std::clamp(zoom, 0.125, 10.0);
}

inline VideoDisplayContentTransform ClampVideoDisplayContentTransform(
	VideoDisplayViewportLayout const& base_viewport,
	VideoDisplayContentTransform transform) {
	transform.zoom = ClampVideoDisplayContentZoom(transform.zoom);
	if (base_viewport.viewport_width <= 0 || base_viewport.viewport_height <= 0) {
		transform.pan_x = 0.0;
		transform.pan_y = 0.0;
		return transform;
	}

	int const content_width = std::max(1, static_cast<int>(std::lround(base_viewport.viewport_width * transform.zoom)));
	int const content_height = std::max(1, static_cast<int>(std::lround(base_viewport.viewport_height * transform.zoom)));
	transform.pan_x = ClampVideoDisplayNormalizedPan(transform.pan_x, content_width, base_viewport.viewport_height);
	transform.pan_y = ClampVideoDisplayNormalizedPan(transform.pan_y, content_height, base_viewport.viewport_height);
	return transform;
}

inline VideoDisplayViewportLayout BuildVideoDisplayContentLayout(
	VideoDisplayViewportLayout const& base_viewport,
	int canvas_height,
	bool enable_content_transform,
	VideoDisplayContentTransform transform = {}) {
	if (!enable_content_transform
		|| base_viewport.viewport_width <= 0
		|| base_viewport.viewport_height <= 0) {
		return base_viewport;
	}

	transform = ClampVideoDisplayContentTransform(base_viewport, transform);
	double const zoom = transform.zoom;
	int const content_width = std::max(1, static_cast<int>(std::lround(base_viewport.viewport_width * zoom)));
	int const content_height = std::max(1, static_cast<int>(std::lround(base_viewport.viewport_height * zoom)));

	double content_left = static_cast<double>(base_viewport.viewport_left)
		+ 0.5 * static_cast<double>(base_viewport.viewport_width - content_width)
		+ transform.pan_x * static_cast<double>(base_viewport.viewport_height);
	double content_top = static_cast<double>(base_viewport.viewport_top)
		+ 0.5 * static_cast<double>(base_viewport.viewport_height - content_height)
		+ transform.pan_y * static_cast<double>(base_viewport.viewport_height);

	VideoDisplayViewportLayout layout;
	layout.viewport_left = static_cast<int>(std::lround(content_left));
	layout.viewport_width = content_width;
	layout.viewport_top = static_cast<int>(std::lround(content_top));
	layout.viewport_height = content_height;
	layout.viewport_bottom = canvas_height - layout.viewport_height - layout.viewport_top;
	return layout;
}

inline Vector2D GetVideoDisplayViewportCenter(VideoDisplayViewportLayout const& base_viewport) {
	return Vector2D(base_viewport.viewport_left, base_viewport.viewport_top)
		+ Vector2D(base_viewport.viewport_width, base_viewport.viewport_height) / 2;
}

inline Vector2D GetVideoDisplayZoomAnchorPoint(
	VideoDisplayViewportLayout const& base_viewport,
	VideoDisplayContentTransform const& transform,
	Vector2D position) {
	if (base_viewport.viewport_height <= 0)
		return Vector2D(0.0f, 0.0f);

	auto clamped = ClampVideoDisplayContentTransform(base_viewport, transform);
	Vector2D const viewport_center = GetVideoDisplayViewportCenter(base_viewport);
	Vector2D const scaled_pan = Vector2D(static_cast<float>(clamped.pan_x), static_cast<float>(clamped.pan_y)) * base_viewport.viewport_height;
	double const zoom = clamped.zoom;
	return (position - viewport_center - scaled_pan) / static_cast<float>(zoom);
}

inline VideoDisplayContentTransform ZoomVideoDisplayContent(
	VideoDisplayViewportLayout const& base_viewport,
	VideoDisplayContentTransform transform,
	double new_zoom,
	Vector2D anchor_point,
	Vector2D new_position) {
	if (base_viewport.viewport_height <= 0) {
		transform.zoom = ClampVideoDisplayContentZoom(new_zoom);
		return transform;
	}

	new_zoom = ClampVideoDisplayContentZoom(new_zoom);
	Vector2D const viewport_center = GetVideoDisplayViewportCenter(base_viewport);
	Vector2D const new_scaled_pan = new_position - viewport_center - anchor_point * static_cast<float>(new_zoom);
	transform.zoom = new_zoom;
	transform.pan_x = new_scaled_pan.X() / base_viewport.viewport_height;
	transform.pan_y = new_scaled_pan.Y() / base_viewport.viewport_height;
	return ClampVideoDisplayContentTransform(base_viewport, transform);
}

inline VideoDisplayContentTransform PanVideoDisplayContent(
	VideoDisplayViewportLayout const& base_viewport,
	VideoDisplayContentTransform transform,
	Vector2D delta) {
	if (base_viewport.viewport_height <= 0)
		return transform;

	transform.pan_x += delta.X() / base_viewport.viewport_height;
	transform.pan_y += delta.Y() / base_viewport.viewport_height;
	return ClampVideoDisplayContentTransform(base_viewport, transform);
}

inline int ResolveVideoDisplayScrollAction(
	bool ctrl_down,
	bool shift_down,
	int scroll_action,
	int ctrl_scroll_action,
	int shift_scroll_action) {
	if (ctrl_down && shift_down)
		return NOTHING;
	if (ctrl_down)
		return ctrl_scroll_action;
	if (shift_down)
		return shift_scroll_action;
	return scroll_action;
}
