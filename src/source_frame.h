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
	Bgra8,
	Nv12,
	P010,
	YCbCr420P8,
	YCbCr420P10,
	YCbCr422P8,
	YCbCr422P10,
	YCbCr444P8,
	YCbCr444P10
};

enum class SourceFrameColorFamily {
	Unknown,
	Rgb,
	YCbCr
};

struct SourceFramePlaneView {
	unsigned char const* data = nullptr;
	ptrdiff_t stride = 0;
	int width = 0;
	int height = 0;
};

struct SourceFramePlaneFormatInfo {
	int width_divisor = 1;
	int height_divisor = 1;
	int components_per_sample = 1;
	int bytes_per_sample = 1;
	int bits_per_component = 8;
};

struct SourceFrameFormatInfo {
	SourceFrameColorFamily color_family = SourceFrameColorFamily::Unknown;
	int plane_count = 0;
	std::array<SourceFramePlaneFormatInfo, 4> planes = { };
};

inline constexpr int DivideRoundUp(int value, int divisor) {
	return (value + divisor - 1) / divisor;
}

inline SourceFrameFormatInfo GetSourceFrameFormatInfo(SourceFramePixelFormat format) {
	switch (format) {
		case SourceFramePixelFormat::Bgra8:
			return { SourceFrameColorFamily::Rgb, 1, { {
				{ 1, 1, 4, 4, 8 }, { }, { }, { }
			} } };
		case SourceFramePixelFormat::Nv12:
			return { SourceFrameColorFamily::YCbCr, 2, { {
				{ 1, 1, 1, 1, 8 },
				{ 2, 2, 2, 2, 8 },
				{ }, { }
			} } };
		case SourceFramePixelFormat::P010:
			return { SourceFrameColorFamily::YCbCr, 2, { {
				{ 1, 1, 1, 2, 10 },
				{ 2, 2, 2, 4, 10 },
				{ }, { }
			} } };
		case SourceFramePixelFormat::YCbCr420P8:
			return { SourceFrameColorFamily::YCbCr, 3, { {
				{ 1, 1, 1, 1, 8 },
				{ 2, 2, 1, 1, 8 },
				{ 2, 2, 1, 1, 8 },
				{ }
			} } };
		case SourceFramePixelFormat::YCbCr420P10:
			return { SourceFrameColorFamily::YCbCr, 3, { {
				{ 1, 1, 1, 2, 10 },
				{ 2, 2, 1, 2, 10 },
				{ 2, 2, 1, 2, 10 },
				{ }
			} } };
		case SourceFramePixelFormat::YCbCr422P8:
			return { SourceFrameColorFamily::YCbCr, 3, { {
				{ 1, 1, 1, 1, 8 },
				{ 2, 1, 1, 1, 8 },
				{ 2, 1, 1, 1, 8 },
				{ }
			} } };
		case SourceFramePixelFormat::YCbCr422P10:
			return { SourceFrameColorFamily::YCbCr, 3, { {
				{ 1, 1, 1, 2, 10 },
				{ 2, 1, 1, 2, 10 },
				{ 2, 1, 1, 2, 10 },
				{ }
			} } };
		case SourceFramePixelFormat::YCbCr444P8:
			return { SourceFrameColorFamily::YCbCr, 3, { {
				{ 1, 1, 1, 1, 8 },
				{ 1, 1, 1, 1, 8 },
				{ 1, 1, 1, 1, 8 },
				{ }
			} } };
		case SourceFramePixelFormat::YCbCr444P10:
			return { SourceFrameColorFamily::YCbCr, 3, { {
				{ 1, 1, 1, 2, 10 },
				{ 1, 1, 1, 2, 10 },
				{ 1, 1, 1, 2, 10 },
				{ }
			} } };
		default:
			return { };
	}
}

inline int GetSourceFramePlaneWidth(SourceFramePixelFormat format, int frame_width, int plane_index) {
	auto info = GetSourceFrameFormatInfo(format);
	if (plane_index < 0 || plane_index >= info.plane_count)
		return 0;
	return DivideRoundUp(frame_width, info.planes[static_cast<size_t>(plane_index)].width_divisor);
}

inline int GetSourceFramePlaneHeight(SourceFramePixelFormat format, int frame_height, int plane_index) {
	auto info = GetSourceFrameFormatInfo(format);
	if (plane_index < 0 || plane_index >= info.plane_count)
		return 0;
	return DivideRoundUp(frame_height, info.planes[static_cast<size_t>(plane_index)].height_divisor);
}

inline char const *SourceFramePixelFormatName(SourceFramePixelFormat format) {
	switch (format) {
		case SourceFramePixelFormat::Bgra8: return "Bgra8";
		case SourceFramePixelFormat::Nv12: return "Nv12";
		case SourceFramePixelFormat::P010: return "P010";
		case SourceFramePixelFormat::YCbCr420P8: return "YCbCr420P8";
		case SourceFramePixelFormat::YCbCr420P10: return "YCbCr420P10";
		case SourceFramePixelFormat::YCbCr422P8: return "YCbCr422P8";
		case SourceFramePixelFormat::YCbCr422P10: return "YCbCr422P10";
		case SourceFramePixelFormat::YCbCr444P8: return "YCbCr444P8";
		case SourceFramePixelFormat::YCbCr444P10: return "YCbCr444P10";
		default: return "Unknown";
	}
}

struct SourceFrame {
	SourceFramePixelFormat pixel_format = SourceFramePixelFormat::Unknown;
	int width = 0;
	int height = 0;
	bool flipped = false;
	int plane_count = 0;
	std::array<SourceFramePlaneView, 4> planes = { };
	SourceFrameColorMetadata color;

	bool IsValid() const {
		auto info = GetSourceFrameFormatInfo(pixel_format);
		if (pixel_format == SourceFramePixelFormat::Unknown
			|| width <= 0
			|| height <= 0
			|| info.plane_count <= 0
			|| plane_count != info.plane_count)
			return false;

		for (int i = 0; i < plane_count; ++i) {
			auto const& plane = planes[static_cast<size_t>(i)];
			auto const& plane_info = info.planes[static_cast<size_t>(i)];
			if (!plane.data || plane.stride == 0)
				return false;
			if (plane.width != GetSourceFramePlaneWidth(pixel_format, width, i)
				|| plane.height != GetSourceFramePlaneHeight(pixel_format, height, i))
				return false;
			ptrdiff_t stride = plane.stride < 0 ? -plane.stride : plane.stride;
			if (stride < static_cast<ptrdiff_t>(plane.width * plane_info.bytes_per_sample))
				return false;
		}

		return true;
	}
};

inline SourceFrame MakeSourceFrameView(VideoFrame const& frame, SourceFrameColorMetadata color = {}) {
	SourceFrame view;
	view.pixel_format = SourceFramePixelFormat::Bgra8;
	view.width = static_cast<int>(frame.width);
	view.height = static_cast<int>(frame.height);
	view.flipped = frame.flipped;
	view.plane_count = GetSourceFrameFormatInfo(view.pixel_format).plane_count;
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
