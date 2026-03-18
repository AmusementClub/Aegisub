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

#include "video_frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

enum class SubtitleOverlayPixelFormat {
	Unknown,
	Bgra8
};

enum class SubtitleOverlayColorRole {
	SubtitleSdrOverlay,
	SubtitleVideoCompatibility
};

enum class SubtitleOverlayCompositionMode {
	Unsupported,
	PremultipliedAlpha,
	OpaqueReplace
};

struct SubtitleOverlayPlaneView {
	unsigned char* data = nullptr;
	ptrdiff_t stride = 0;
	int width = 0;
	int height = 0;
};

struct SubtitleOverlayDirtyRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct SubtitleOverlayRowRange {
	int x0 = 0;
	int x1 = 0;

	bool IsEmpty() const {
		return x0 >= x1;
	}
};

struct SubtitleOverlay {
	SubtitleOverlayPixelFormat pixel_format = SubtitleOverlayPixelFormat::Unknown;
	int width = 0;
	int height = 0;
	int canvas_width = 0;
	int canvas_height = 0;
	int target_x = 0;
	int target_y = 0;
	bool flipped = false;
	bool premultiplied_alpha = false;
	int plane_count = 0;
	std::array<SubtitleOverlayPlaneView, 4> planes = { };
	SubtitleOverlayDirtyRect const* dirty_rects = nullptr;
	int dirty_rect_count = 0;
	bool has_visible_content = false;
	bool force_full_upload = false;
	SubtitleOverlayColorRole color_role = SubtitleOverlayColorRole::SubtitleSdrOverlay;
	SubtitleOverlayCompositionMode composition_mode = SubtitleOverlayCompositionMode::Unsupported;
	std::string nominal_color_space = "BT.709";

	bool IsValid() const {
		return pixel_format != SubtitleOverlayPixelFormat::Unknown
			&& width > 0
			&& height > 0
			&& canvas_width >= width
			&& canvas_height >= height
			&& plane_count > 0
			&& planes[0].data != nullptr;
	}

	bool IsDirectRenderable() const {
		return composition_mode == SubtitleOverlayCompositionMode::PremultipliedAlpha
			|| composition_mode == SubtitleOverlayCompositionMode::OpaqueReplace;
	}
};

struct SubtitleOverlayStorage {
	std::vector<unsigned char> pixels;
	std::vector<SubtitleOverlayDirtyRect> dirty_rects;
	std::vector<SubtitleOverlayRowRange> row_ranges;
	int width = 0;
	int height = 0;
	size_t pitch = 0;
	bool flipped = false;
	bool has_visible_content = false;
	int active_row_begin = 0;
	int active_row_end = 0;

	void Reset(int new_width, int new_height, bool new_flipped, bool clear_pixels = true) {
		bool geometry_changed = width != new_width || height != new_height || flipped != new_flipped;
		width = new_width;
		height = new_height;
		flipped = new_flipped;
		pitch = static_cast<size_t>(new_width) * 4;
		if (geometry_changed) {
			pixels.resize(pitch * static_cast<size_t>(new_height));
			row_ranges.resize(static_cast<size_t>(new_height));
		}
		if (clear_pixels)
			std::fill(pixels.begin(), pixels.end(), 0);
		std::fill(row_ranges.begin(), row_ranges.end(), SubtitleOverlayRowRange{});
		dirty_rects.clear();
		has_visible_content = false;
		active_row_begin = new_height;
		active_row_end = 0;
	}

	SubtitleOverlay MakeView(bool make_premultiplied_overlay = false) {
		SubtitleOverlay overlay;
		overlay.pixel_format = SubtitleOverlayPixelFormat::Bgra8;
		overlay.width = width;
		overlay.height = height;
		overlay.canvas_width = width;
		overlay.canvas_height = height;
		overlay.flipped = flipped;
		overlay.premultiplied_alpha = make_premultiplied_overlay;
		overlay.composition_mode = make_premultiplied_overlay
			? SubtitleOverlayCompositionMode::PremultipliedAlpha
			: SubtitleOverlayCompositionMode::Unsupported;
		overlay.plane_count = 1;
		overlay.planes[0] = {
			pixels.data(),
			static_cast<ptrdiff_t>(pitch),
			width,
			height
		};
		overlay.dirty_rects = dirty_rects.empty() ? nullptr : dirty_rects.data();
		overlay.dirty_rect_count = static_cast<int>(dirty_rects.size());
		overlay.has_visible_content = has_visible_content;
		return overlay;
	}
};

inline SubtitleOverlay MakeLegacyBgraSubtitleOverlayView(VideoFrame& frame) {
	SubtitleOverlay overlay;
	overlay.pixel_format = SubtitleOverlayPixelFormat::Bgra8;
	overlay.width = static_cast<int>(frame.width);
	overlay.height = static_cast<int>(frame.height);
	overlay.canvas_width = static_cast<int>(frame.width);
	overlay.canvas_height = static_cast<int>(frame.height);
	overlay.flipped = frame.flipped;
	overlay.plane_count = 1;
	overlay.planes[0] = {
		frame.data.data(),
		static_cast<ptrdiff_t>(frame.pitch),
		static_cast<int>(frame.width),
		static_cast<int>(frame.height)
	};
	return overlay;
}
