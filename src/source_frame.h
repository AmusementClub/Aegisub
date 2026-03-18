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

enum class SourceFrameOutputMode {
	Bgra8,
	Native
};

enum class SourceFrameNativeFormatNamespace {
	None,
	FFmpegAVPixelFormat
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

struct SourceFrameNativeFormatIdentity {
	SourceFrameNativeFormatNamespace format_namespace = SourceFrameNativeFormatNamespace::None;
	int format_id = -1;

	bool IsValid() const {
		return format_namespace != SourceFrameNativeFormatNamespace::None && format_id >= 0;
	}
};

inline constexpr int DivideRoundUp(int value, int divisor) {
	return (value + divisor - 1) / divisor;
}

inline bool SourceFramePlaneFormatInfoEquals(
	SourceFramePlaneFormatInfo const& lhs,
	SourceFramePlaneFormatInfo const& rhs) {
	return lhs.width_divisor == rhs.width_divisor
		&& lhs.height_divisor == rhs.height_divisor
		&& lhs.components_per_sample == rhs.components_per_sample
		&& lhs.bytes_per_sample == rhs.bytes_per_sample
		&& lhs.bits_per_component == rhs.bits_per_component;
}

inline bool SourceFrameFormatInfoEquals(
	SourceFrameFormatInfo const& lhs,
	SourceFrameFormatInfo const& rhs) {
	if (lhs.color_family != rhs.color_family || lhs.plane_count != rhs.plane_count)
		return false;
	for (int i = 0; i < lhs.plane_count; ++i) {
		if (!SourceFramePlaneFormatInfoEquals(
			lhs.planes[static_cast<size_t>(i)],
			rhs.planes[static_cast<size_t>(i)]))
			return false;
	}
	return true;
}

inline bool IsValidSourceFrameFormatInfo(SourceFrameFormatInfo const& info) {
	if (info.plane_count <= 0 || info.plane_count > 4)
		return false;
	if (info.color_family == SourceFrameColorFamily::Unknown)
		return false;

	for (int i = 0; i < info.plane_count; ++i) {
		auto const& plane = info.planes[static_cast<size_t>(i)];
		if (plane.width_divisor <= 0
			|| plane.height_divisor <= 0
			|| plane.components_per_sample <= 0
			|| plane.bytes_per_sample <= 0
			|| plane.bits_per_component <= 0)
			return false;
	}

	return true;
}

inline SourceFrameFormatInfo MakeBgra8SourceFrameFormatInfo() {
	return { SourceFrameColorFamily::Rgb, 1, { {
		{ 1, 1, 4, 4, 8 }, { }, { }, { }
	} } };
}

inline SourceFrameFormatInfo MakeSemiplanar420SourceFrameFormatInfo(
	int bits_per_component,
	int luma_bytes_per_sample,
	int chroma_bytes_per_sample) {
	return { SourceFrameColorFamily::YCbCr, 2, { {
		{ 1, 1, 1, luma_bytes_per_sample, bits_per_component },
		{ 2, 2, 2, chroma_bytes_per_sample, bits_per_component },
		{ }, { }
	} } };
}

inline SourceFrameFormatInfo MakePlanarYCbCrSourceFrameFormatInfo(
	int chroma_width_divisor,
	int chroma_height_divisor,
	int bits_per_component,
	int bytes_per_sample) {
	return { SourceFrameColorFamily::YCbCr, 3, { {
		{ 1, 1, 1, bytes_per_sample, bits_per_component },
		{ chroma_width_divisor, chroma_height_divisor, 1, bytes_per_sample, bits_per_component },
		{ chroma_width_divisor, chroma_height_divisor, 1, bytes_per_sample, bits_per_component },
		{ }
	} } };
}

inline int GetSourceFramePlaneWidth(SourceFrameFormatInfo const& info, int frame_width, int plane_index) {
	if (plane_index < 0 || plane_index >= info.plane_count)
		return 0;
	return DivideRoundUp(frame_width, info.planes[static_cast<size_t>(plane_index)].width_divisor);
}

inline int GetSourceFramePlaneHeight(SourceFrameFormatInfo const& info, int frame_height, int plane_index) {
	if (plane_index < 0 || plane_index >= info.plane_count)
		return 0;
	return DivideRoundUp(frame_height, info.planes[static_cast<size_t>(plane_index)].height_divisor);
}

inline char const *SourceFramePixelFormatName(SourceFramePixelFormat format) {
	switch (format) {
		case SourceFramePixelFormat::Bgra8: return "Bgra8";
		default: return "Unknown";
	}
}

inline char const *SourceFrameOutputModeName(SourceFrameOutputMode mode) {
	switch (mode) {
		case SourceFrameOutputMode::Bgra8: return "Bgra8";
		case SourceFrameOutputMode::Native: return "Native";
		default: return "Unknown";
	}
}

struct SourceFrame {
	SourceFramePixelFormat pixel_format = SourceFramePixelFormat::Unknown;
	SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;
	SourceFrameNativeFormatIdentity native_format;
	SourceFrameFormatInfo format_info;
	int width = 0;
	int height = 0;
	bool flipped = false;
	int plane_count = 0;
	std::array<SourceFramePlaneView, 4> planes = { };
	SourceFrameColorMetadata color;

	bool IsValid() const {
		if (pixel_format == SourceFramePixelFormat::Unknown && output_mode != SourceFrameOutputMode::Native)
			return false;
		if (output_mode == SourceFrameOutputMode::Bgra8) {
			if (pixel_format != SourceFramePixelFormat::Bgra8
				|| native_format.IsValid()
				|| !SourceFrameFormatInfoEquals(format_info, MakeBgra8SourceFrameFormatInfo()))
				return false;
		}
		else if (output_mode == SourceFrameOutputMode::Native) {
			if (pixel_format != SourceFramePixelFormat::Unknown || !native_format.IsValid())
				return false;
		}
		else {
			return false;
		}

		if (width <= 0
			|| height <= 0
			|| !IsValidSourceFrameFormatInfo(format_info)
			|| plane_count != format_info.plane_count)
			return false;

		for (int i = 0; i < plane_count; ++i) {
			auto const& plane = planes[static_cast<size_t>(i)];
			auto const& plane_info = format_info.planes[static_cast<size_t>(i)];
			if (!plane.data || plane.stride == 0)
				return false;
			if (plane.width != GetSourceFramePlaneWidth(format_info, width, i)
				|| plane.height != GetSourceFramePlaneHeight(format_info, height, i))
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
	view.output_mode = SourceFrameOutputMode::Bgra8;
	view.format_info = MakeBgra8SourceFrameFormatInfo();
	view.width = static_cast<int>(frame.width);
	view.height = static_cast<int>(frame.height);
	view.flipped = frame.flipped;
	view.plane_count = view.format_info.plane_count;
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
