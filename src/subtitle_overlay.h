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

#include <array>
#include <cstddef>
#include <string>

enum class SubtitleOverlayPixelFormat {
	Unknown,
	Bgra8
};

enum class SubtitleOverlayColorRole {
	SubtitleSdrOverlay,
	SubtitleVideoCompatibility
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
	bool flipped = false;
	bool premultiplied_alpha = false;
	int plane_count = 0;
	std::array<SubtitleOverlayPlaneView, 4> planes = { };
	SubtitleOverlayColorRole color_role = SubtitleOverlayColorRole::SubtitleSdrOverlay;
	std::string nominal_color_space = "BT.709";

	bool IsValid() const {
		return pixel_format != SubtitleOverlayPixelFormat::Unknown
			&& width > 0
			&& height > 0
			&& plane_count > 0
			&& planes[0].data != nullptr;
	}
};

inline SubtitleOverlay MakeLegacyBgraSubtitleOverlayView(VideoFrame& frame) {
	SubtitleOverlay overlay;
	overlay.pixel_format = SubtitleOverlayPixelFormat::Bgra8;
	overlay.width = static_cast<int>(frame.width);
	overlay.height = static_cast<int>(frame.height);
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
