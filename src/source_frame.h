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

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

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

enum class SourceFrameChromaLocation {
	Unknown,
	Left,
	Center,
	TopLeft,
	TopCenter,
	BottomLeft,
	BottomCenter
};

struct SourceFramePlaneView {
	unsigned char const* data = nullptr;
	ptrdiff_t stride = 0;
	int width = 0;
	int height = 0;
};

struct SourceFrameRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	bool IsValid() const {
		return width > 0 && height > 0;
	}
};

struct SourceFrameGeometry {
	int storage_width = 0;
	int storage_height = 0;
	SourceFrameRect visible_rect;
	int rotation = 0;
	bool display_vflip = false;
	double pixel_aspect_ratio = 1.0;
};

struct SourceFramePlaneFormatInfo {
	int width_divisor = 1;
	int height_divisor = 1;
	int components_per_sample = 1;
	int bytes_per_sample = 1;
	int bits_per_component = 8;
	std::array<int, 4> component_shift = { { 0, 0, 0, 0 } };
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

enum class SourceFrameNativePayloadKind {
	None,
	FFmpegAVFrame
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
		&& lhs.bits_per_component == rhs.bits_per_component
		&& lhs.component_shift == rhs.component_shift;
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

inline bool SourceFrameHasSubsampledChroma(SourceFrameFormatInfo const& info) {
	if (info.color_family != SourceFrameColorFamily::YCbCr || info.plane_count <= 1)
		return false;

	for (int i = 1; i < info.plane_count; ++i) {
		auto const& plane = info.planes[static_cast<size_t>(i)];
		if (plane.width_divisor > 1 || plane.height_divisor > 1)
			return true;
	}

	return false;
}

inline SourceFrameFormatInfo MakeBgra8SourceFrameFormatInfo() {
	return { SourceFrameColorFamily::Rgb, 1, { {
		{ 1, 1, 4, 4, 8, { { 16, 8, 0, 24 } } }, { }, { }, { }
	} } };
}

inline SourceFrameFormatInfo MakeSemiplanar420SourceFrameFormatInfo(
	int bits_per_component,
	int luma_bytes_per_sample,
	int chroma_bytes_per_sample,
	std::array<int, 4> luma_component_shift = { { 0, 0, 0, 0 } },
	std::array<int, 4> chroma_component_shift = { { 0, 8, 0, 0 } }) {
	return { SourceFrameColorFamily::YCbCr, 2, { {
		{ 1, 1, 1, luma_bytes_per_sample, bits_per_component, luma_component_shift },
		{ 2, 2, 2, chroma_bytes_per_sample, bits_per_component, chroma_component_shift },
		{ }, { }
	} } };
}

inline SourceFrameFormatInfo MakePlanarYCbCrSourceFrameFormatInfo(
	int chroma_width_divisor,
	int chroma_height_divisor,
	int bits_per_component,
	int bytes_per_sample,
	int component_shift = 0) {
	return { SourceFrameColorFamily::YCbCr, 3, { {
		{ 1, 1, 1, bytes_per_sample, bits_per_component, { { component_shift, 0, 0, 0 } } },
		{ chroma_width_divisor, chroma_height_divisor, 1, bytes_per_sample, bits_per_component, { { component_shift, 0, 0, 0 } } },
		{ chroma_width_divisor, chroma_height_divisor, 1, bytes_per_sample, bits_per_component, { { component_shift, 0, 0, 0 } } },
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

inline SourceFrameGeometry MakeDefaultSourceFrameGeometry(int width, int height) {
	SourceFrameGeometry geometry;
	geometry.storage_width = width;
	geometry.storage_height = height;
	geometry.visible_rect = { 0, 0, width, height };
	return geometry;
}

inline SourceFrameRect ClampSourceFrameRectToBounds(SourceFrameRect const& rect, int width, int height) {
	if (width <= 0 || height <= 0)
		return { };

	int x0 = std::clamp(rect.x, 0, width);
	int y0 = std::clamp(rect.y, 0, height);
	int x1 = std::clamp(rect.x + rect.width, 0, width);
	int y1 = std::clamp(rect.y + rect.height, 0, height);
	if (x0 >= x1 || y0 >= y1)
		return { };

	return { x0, y0, x1 - x0, y1 - y0 };
}

inline SourceFrameRect GetSourceFrameVisibleRect(SourceFrameGeometry const& geometry, int fallback_width, int fallback_height) {
	int storage_width = geometry.storage_width > 0 ? geometry.storage_width : fallback_width;
	int storage_height = geometry.storage_height > 0 ? geometry.storage_height : fallback_height;
	if (storage_width <= 0 || storage_height <= 0)
		return { };

	SourceFrameRect full_rect = { 0, 0, storage_width, storage_height };
	if (!geometry.visible_rect.IsValid())
		return full_rect;

	auto clamped = ClampSourceFrameRectToBounds(geometry.visible_rect, storage_width, storage_height);
	if (!clamped.IsValid())
		return full_rect;
	return clamped;
}

struct SourceFrame {
	SourceFramePixelFormat pixel_format = SourceFramePixelFormat::Unknown;
	SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;
	SourceFrameNativeFormatIdentity native_format;
	SourceFrameNativePayloadKind native_payload_kind = SourceFrameNativePayloadKind::None;
	void const* native_payload = nullptr;
	SourceFrameFormatInfo format_info;
	int width = 0;
	int height = 0;
	bool flipped = false;
	int plane_count = 0;
	std::array<SourceFramePlaneView, 4> planes = { };
	SourceFrameColorMetadata color;
	SourceFrameChromaLocation chroma_location = SourceFrameChromaLocation::Unknown;
	SourceFrameGeometry geometry;

	bool IsValid() const {
		if (pixel_format == SourceFramePixelFormat::Unknown && output_mode != SourceFrameOutputMode::Native)
			return false;
		if (output_mode == SourceFrameOutputMode::Bgra8) {
			if (pixel_format != SourceFramePixelFormat::Bgra8
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

inline SourceFrameRect GetSourceFrameVisibleRect(SourceFrame const& frame) {
	return GetSourceFrameVisibleRect(frame.geometry, frame.width, frame.height);
}

inline int NormalizeSourceFrameRotationDegrees(int rotation) {
	int normalized = rotation % 360;
	if (normalized < 0)
		normalized += 360;
	return normalized;
}

inline SourceFrameRect TransformSourceFrameRectToDisplaySpace(
	SourceFrameRect const& rect,
	int source_width,
	int source_height,
	int rotation,
	bool display_vflip) {
	auto clamped = ClampSourceFrameRectToBounds(rect, source_width, source_height);
	if (!clamped.IsValid())
		return { };

	int normalized = NormalizeSourceFrameRotationDegrees(rotation);
	int output_width = (normalized == 90 || normalized == 270) ? source_height : source_width;
	int output_height = (normalized == 90 || normalized == 270) ? source_width : source_height;

	auto transform_point = [&](int x, int y) -> std::pair<int, int> {
		switch (normalized) {
			case 90:
			{
				int original_x = x;
				x = source_height - y;
				y = original_x;
				break;
			}
			case 180:
				x = source_width - x;
				y = source_height - y;
				break;
			case 270:
			{
				int original_x = x;
				x = y;
				y = source_width - original_x;
				break;
			}
			default:
				break;
		}

		if (display_vflip)
			y = output_height - y;

		return { x, y };
	};

	int x0 = clamped.x;
	int y0 = clamped.y;
	int x1 = clamped.x + clamped.width;
	int y1 = clamped.y + clamped.height;

	auto p0 = transform_point(x0, y0);
	auto p1 = transform_point(x1, y0);
	auto p2 = transform_point(x1, y1);
	auto p3 = transform_point(x0, y1);

	int out_x0 = std::min(std::min(p0.first, p1.first), std::min(p2.first, p3.first));
	int out_y0 = std::min(std::min(p0.second, p1.second), std::min(p2.second, p3.second));
	int out_x1 = std::max(std::max(p0.first, p1.first), std::max(p2.first, p3.first));
	int out_y1 = std::max(std::max(p0.second, p1.second), std::max(p2.second, p3.second));
	return { out_x0, out_y0, out_x1 - out_x0, out_y1 - out_y0 };
}

inline SourceFrameGeometry BakeSourceFrameGeometry(SourceFrameGeometry const& geometry) {
	int storage_width = geometry.storage_width;
	int storage_height = geometry.storage_height;
	if (storage_width <= 0 || storage_height <= 0)
		return geometry;

	SourceFrameGeometry baked;
	int normalized = NormalizeSourceFrameRotationDegrees(geometry.rotation);
	bool swap_axes = normalized == 90 || normalized == 270;
	baked.storage_width = swap_axes ? storage_height : storage_width;
	baked.storage_height = swap_axes ? storage_width : storage_height;
	baked.visible_rect = TransformSourceFrameRectToDisplaySpace(
		GetSourceFrameVisibleRect(geometry, storage_width, storage_height),
		storage_width,
		storage_height,
		geometry.rotation,
		geometry.display_vflip);
	if (!baked.visible_rect.IsValid())
		baked.visible_rect = { 0, 0, baked.storage_width, baked.storage_height };
	baked.rotation = 0;
	baked.display_vflip = false;

	double pixel_aspect_ratio = geometry.pixel_aspect_ratio > 0.0 ? geometry.pixel_aspect_ratio : 1.0;
	if (swap_axes && pixel_aspect_ratio != 0.0)
		baked.pixel_aspect_ratio = 1.0 / pixel_aspect_ratio;
	else
		baked.pixel_aspect_ratio = pixel_aspect_ratio;
	return baked;
}

inline SourceFrameRect GetSourceFrameDisplayOutputRect(SourceFrameGeometry const& geometry) {
	auto baked = BakeSourceFrameGeometry(geometry);
	auto visible = GetSourceFrameVisibleRect(baked, baked.storage_width, baked.storage_height);
	if (visible.IsValid())
		return visible;
	return { 0, 0, baked.storage_width, baked.storage_height };
}

inline double GetSourceFrameDisplayAspectRatio(SourceFrameGeometry const& geometry) {
	auto baked = BakeSourceFrameGeometry(geometry);
	auto visible = GetSourceFrameDisplayOutputRect(geometry);
	if (visible.width <= 0 || visible.height <= 0)
		return 0.0;
	double pixel_aspect_ratio = baked.pixel_aspect_ratio > 0.0 ? baked.pixel_aspect_ratio : 1.0;
	return static_cast<double>(visible.width) * pixel_aspect_ratio / visible.height;
}

inline bool SourceFrameHasSupportedQuarterTurnRotation(SourceFrameGeometry const& geometry) {
	switch (NormalizeSourceFrameRotationDegrees(geometry.rotation)) {
		case 0:
		case 90:
		case 180:
		case 270:
			return true;
		default:
			return false;
	}
}

inline bool SourceFrameHasSupportedQuarterTurnRotation(SourceFrame const& frame) {
	return SourceFrameHasSupportedQuarterTurnRotation(frame.geometry);
}

inline bool SourceFrameHasUnbakedDisplayTransform(SourceFrame const& frame) {
	return frame.output_mode == SourceFrameOutputMode::Native;
}

inline bool SourceFrameNeedsDisplayTransformFallback(SourceFrame const& frame) {
	return !SourceFrameHasSupportedQuarterTurnRotation(frame);
}

inline bool SourceFrameHasSubsampledChroma(SourceFrame const& frame) {
	return SourceFrameHasSubsampledChroma(frame.format_info);
}

inline SourceFrame MakeSourceFrameView(VideoFrame const& frame, SourceFrameColorMetadata color = {}) {
	SourceFrame view;
	view.pixel_format = SourceFramePixelFormat::Bgra8;
	view.output_mode = SourceFrameOutputMode::Bgra8;
	view.format_info = MakeBgra8SourceFrameFormatInfo();
	view.width = static_cast<int>(frame.width);
	view.height = static_cast<int>(frame.height);
	view.flipped = frame.flipped;
	view.plane_count = view.format_info.plane_count;
	view.geometry = MakeDefaultSourceFrameGeometry(view.width, view.height);
	view.planes[0] = {
		frame.data.data(),
		static_cast<ptrdiff_t>(frame.pitch),
		static_cast<int>(frame.width),
		static_cast<int>(frame.height)
	};
	view.color = std::move(color);
	return view;
}

inline SourceFrame MakeSourceFrameView(VideoFrame const& frame, SourceFrame const& reference) {
	auto view = MakeSourceFrameView(frame, reference.color);
	view.native_format = reference.native_format;
	view.chroma_location = reference.chroma_location;
	view.geometry = reference.geometry;
	return view;
}

inline SourceFrame MakeBakedSourceFrameView(VideoFrame const& frame, SourceFrame const& reference) {
	auto view = MakeSourceFrameView(frame, reference.color);
	view.native_format = reference.native_format;
	view.chroma_location = reference.chroma_location;
	view.geometry = BakeSourceFrameGeometry(reference.geometry);
	return view;
}

inline SourceFrame MakeSourceFrameView(VideoFrame const& frame, std::string matrix) {
	return MakeSourceFrameView(frame, SourceFrameColorMetadataFromLegacyColorSpace(std::move(matrix)));
}
