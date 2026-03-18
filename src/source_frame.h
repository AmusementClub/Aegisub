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

#include "include/aegisub/video_color_metadata.h"
#include "video_frame.h"

#include <array>
#include <cstddef>

enum class SourceFramePixelFormat {
	Unknown,
	Bgra8
};

struct SourceFramePlaneView {
	unsigned char const* data = nullptr;
	ptrdiff_t stride = 0;
	int width = 0;
	int height = 0;
};

struct SourceFrame {
	SourceFramePixelFormat pixel_format = SourceFramePixelFormat::Unknown;
	int width = 0;
	int height = 0;
	bool flipped = false;
	int plane_count = 0;
	std::array<SourceFramePlaneView, 4> planes = { };
	SourceFrameColorMetadata color;

	bool IsValid() const {
		return pixel_format != SourceFramePixelFormat::Unknown
			&& width > 0
			&& height > 0
			&& plane_count > 0
			&& planes[0].data != nullptr;
	}
};

inline SourceFrame MakeSourceFrameView(VideoFrame const& frame, SourceFrameColorMetadata color = {}) {
	SourceFrame view;
	view.pixel_format = SourceFramePixelFormat::Bgra8;
	view.width = static_cast<int>(frame.width);
	view.height = static_cast<int>(frame.height);
	view.flipped = frame.flipped;
	view.plane_count = 1;
	view.planes[0] = {
		frame.data.data(),
		static_cast<ptrdiff_t>(frame.pitch),
		static_cast<int>(frame.width),
		static_cast<int>(frame.height)
	};
	view.color = std::move(color);
	return view;
}

inline SourceFrame MakeSourceFrameView(VideoFrame const& frame, std::string matrix) {
	return MakeSourceFrameView(frame, SourceFrameColorMetadataFromLegacyColorSpace(std::move(matrix)));
}
