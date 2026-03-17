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
	int width = 0;
	int height = 0;
	size_t pitch = 0;
	bool flipped = false;

	void Reset(int new_width, int new_height, bool new_flipped) {
		width = new_width;
		height = new_height;
		flipped = new_flipped;
		pitch = static_cast<size_t>(new_width) * 4;
		pixels.resize(pitch * static_cast<size_t>(new_height));
		std::fill(pixels.begin(), pixels.end(), 0);
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
