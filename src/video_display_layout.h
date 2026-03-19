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

struct VideoDisplayViewportLayout {
	int viewport_left = 0;
	int viewport_width = 0;
	int viewport_bottom = 0;
	int viewport_top = 0;
	int viewport_height = 0;
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
