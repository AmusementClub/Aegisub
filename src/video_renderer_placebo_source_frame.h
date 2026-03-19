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

#include <libplacebo/colorspace.h>
#include <libplacebo/utils/upload.h>

#include <algorithm>
#include <cctype>
#include <string>

inline std::string NormalizePlaceboColorToken(std::string value) {
	std::string normalized;
	normalized.reserve(value.size());
	for (unsigned char ch : value) {
		if (std::isalnum(ch))
			normalized.push_back(static_cast<char>(std::toupper(ch)));
	}
	return normalized;
}

inline bool PlaceboColorContainsToken(std::string const& value, char const *token) {
	return value.find(token) != std::string::npos;
}

inline enum pl_color_primaries InferPlaceboPrimaries(SourceFrameColorMetadata const& color) {
	auto token = NormalizePlaceboColorToken(color.primaries);
	if (PlaceboColorContainsToken(token, "2020"))
		return PL_COLOR_PRIM_BT_2020;
	if (PlaceboColorContainsToken(token, "FILM"))
		return PL_COLOR_PRIM_FILM_C;
	if (PlaceboColorContainsToken(token, "470M"))
		return PL_COLOR_PRIM_BT_470M;
	if (PlaceboColorContainsToken(token, "601525") || PlaceboColorContainsToken(token, "170M") || PlaceboColorContainsToken(token, "240M"))
		return PL_COLOR_PRIM_BT_601_525;
	if (PlaceboColorContainsToken(token, "601625") || PlaceboColorContainsToken(token, "470BG") || PlaceboColorContainsToken(token, "BT601"))
		return PL_COLOR_PRIM_BT_601_625;
	if (PlaceboColorContainsToken(token, "DISPLAYP3"))
		return PL_COLOR_PRIM_DISPLAY_P3;
	if (PlaceboColorContainsToken(token, "DCIP3"))
		return PL_COLOR_PRIM_DCI_P3;
	return PL_COLOR_PRIM_BT_709;
}

inline enum pl_color_transfer InferPlaceboTransfer(SourceFrameColorMetadata const& color) {
	auto token = NormalizePlaceboColorToken(color.transfer);
	if (PlaceboColorContainsToken(token, "LINEAR"))
		return PL_COLOR_TRC_LINEAR;
	if (PlaceboColorContainsToken(token, "SRGB"))
		return PL_COLOR_TRC_SRGB;
	if (PlaceboColorContainsToken(token, "GAMMA18"))
		return PL_COLOR_TRC_GAMMA18;
	if (PlaceboColorContainsToken(token, "GAMMA20"))
		return PL_COLOR_TRC_GAMMA20;
	if (PlaceboColorContainsToken(token, "GAMMA22"))
		return PL_COLOR_TRC_GAMMA22;
	if (PlaceboColorContainsToken(token, "GAMMA24"))
		return PL_COLOR_TRC_GAMMA24;
	if (PlaceboColorContainsToken(token, "GAMMA26"))
		return PL_COLOR_TRC_GAMMA26;
	if (PlaceboColorContainsToken(token, "GAMMA28"))
		return PL_COLOR_TRC_GAMMA28;
	if (PlaceboColorContainsToken(token, "PQ") || PlaceboColorContainsToken(token, "2084"))
		return PL_COLOR_TRC_PQ;
	if (PlaceboColorContainsToken(token, "HLG"))
		return PL_COLOR_TRC_HLG;
	return PL_COLOR_TRC_BT_1886;
}

inline enum pl_color_levels InferPlaceboLevels(SourceFrameColorMetadata const& color) {
	if (color.range == SourceFrameColorRange::Limited)
		return PL_COLOR_LEVELS_LIMITED;
	return PL_COLOR_LEVELS_FULL;
}

inline enum pl_color_system InferPlaceboColorSystem(SourceFrame const& frame) {
	if (frame.output_mode == SourceFrameOutputMode::Bgra8 || frame.format_info.color_family == SourceFrameColorFamily::Rgb)
		return PL_COLOR_SYSTEM_RGB;

	auto token = NormalizePlaceboColorToken(frame.color.matrix);
	if (PlaceboColorContainsToken(token, "2020CL"))
		return PL_COLOR_SYSTEM_BT_2020_C;
	if (PlaceboColorContainsToken(token, "2020"))
		return PL_COLOR_SYSTEM_BT_2020_NC;
	if (PlaceboColorContainsToken(token, "240M"))
		return PL_COLOR_SYSTEM_SMPTE_240M;
	if (PlaceboColorContainsToken(token, "601") || PlaceboColorContainsToken(token, "470BG") || PlaceboColorContainsToken(token, "170M"))
		return PL_COLOR_SYSTEM_BT_601;
	if (PlaceboColorContainsToken(token, "NONE") || PlaceboColorContainsToken(token, "RGB"))
		return PL_COLOR_SYSTEM_RGB;
	return PL_COLOR_SYSTEM_BT_709;
}

inline struct pl_color_repr BuildPlaceboSourceFrameRepr(SourceFrame const& frame) {
	struct pl_color_repr repr = {};
	repr.sys = InferPlaceboColorSystem(frame);
	repr.levels = InferPlaceboLevels(frame.color);
	repr.alpha = PL_ALPHA_NONE;

	if (frame.output_mode == SourceFrameOutputMode::Bgra8) {
		repr.bits = { 8, 8, 0 };
		return repr;
	}

	auto const& first_plane = frame.format_info.planes[0];
	int bytes_per_component = std::max(1, first_plane.bytes_per_sample / std::max(1, first_plane.components_per_sample));
	repr.bits = {
		bytes_per_component * 8,
		first_plane.bits_per_component,
		first_plane.component_shift[0]
	};
	return repr;
}

inline struct pl_rect2df BuildPlaceboSourceFrameCropRect(SourceFrame const& frame) {
	struct pl_rect2df full = {
		0.0f,
		0.0f,
		static_cast<float>(frame.width),
		static_cast<float>(frame.height)
	};

	if (frame.width <= 0 || frame.height <= 0)
		return full;

	auto const& visible = frame.geometry.visible_rect;
	if (!visible.IsValid())
		return full;

	int x0 = std::clamp(visible.x, 0, frame.width);
	int y0 = std::clamp(visible.y, 0, frame.height);
	int x1 = std::clamp(visible.x + visible.width, 0, frame.width);
	int y1 = std::clamp(visible.y + visible.height, 0, frame.height);
	if (x0 >= x1 || y0 >= y1)
		return full;

	return {
		static_cast<float>(x0),
		static_cast<float>(y0),
		static_cast<float>(x1),
		static_cast<float>(y1)
	};
}

inline struct pl_color_space BuildPlaceboSourceFrameColorSpace(SourceFrame const& frame) {
	struct pl_color_space space = {};
	space.primaries = InferPlaceboPrimaries(frame.color);
	space.transfer = InferPlaceboTransfer(frame.color);
	return space;
}

inline enum pl_chroma_location InferPlaceboChromaLocation(SourceFrameChromaLocation chroma_location) {
	switch (chroma_location) {
		case SourceFrameChromaLocation::Left:
			return PL_CHROMA_LEFT;
		case SourceFrameChromaLocation::Center:
			return PL_CHROMA_CENTER;
		case SourceFrameChromaLocation::TopLeft:
			return PL_CHROMA_TOP_LEFT;
		case SourceFrameChromaLocation::TopCenter:
			return PL_CHROMA_TOP_CENTER;
		case SourceFrameChromaLocation::BottomLeft:
			return PL_CHROMA_BOTTOM_LEFT;
		case SourceFrameChromaLocation::BottomCenter:
			return PL_CHROMA_BOTTOM_CENTER;
		case SourceFrameChromaLocation::Unknown:
		default:
			return PL_CHROMA_UNKNOWN;
	}
}

inline enum pl_chroma_location ResolvePlaceboChromaLocation(SourceFrame const& frame) {
	if (frame.chroma_location != SourceFrameChromaLocation::Unknown)
		return InferPlaceboChromaLocation(frame.chroma_location);

	if (!SourceFrameHasSubsampledChroma(frame))
		return PL_CHROMA_CENTER;

	return frame.color.range == SourceFrameColorRange::Full
		? PL_CHROMA_CENTER
		: PL_CHROMA_LEFT;
}

inline bool PlaceboSourceFrameNeedsExplicitChromaLocation(SourceFrame const& frame) {
	return frame.output_mode == SourceFrameOutputMode::Native
		&& frame.format_info.color_family == SourceFrameColorFamily::YCbCr
		&& SourceFrameHasSubsampledChroma(frame);
}

inline struct pl_color_repr BuildPlaceboRenderTargetRepr() {
	struct pl_color_repr repr = {};
	repr.sys = PL_COLOR_SYSTEM_RGB;
	repr.levels = PL_COLOR_LEVELS_FULL;
	repr.alpha = PL_ALPHA_NONE;
	repr.bits = { 8, 8, 0 };
	return repr;
}

inline bool BuildPlaceboNativePlaneData(SourceFrame const& frame, int plane_index, struct pl_plane_data& data) {
	if (!frame.IsValid()
		|| frame.output_mode != SourceFrameOutputMode::Native
		|| plane_index < 0
		|| plane_index >= frame.plane_count)
		return false;

	auto const& plane = frame.planes[static_cast<size_t>(plane_index)];
	auto const& plane_info = frame.format_info.planes[static_cast<size_t>(plane_index)];
	auto* pixels = plane.data;
	ptrdiff_t stride = plane.stride;
	size_t row_stride = static_cast<size_t>(stride < 0 ? -stride : stride);
	if (!pixels || row_stride == 0)
		return false;
	if (stride < 0)
		pixels += static_cast<ptrdiff_t>(plane.height - 1) * row_stride;

	data = {};
	data.type = PL_FMT_UNORM;
	data.width = plane.width;
	data.height = plane.height;
	data.pixel_stride = plane_info.bytes_per_sample;
	data.row_stride = row_stride;
	data.pixels = pixels;
	for (int i = 0; i < plane_info.components_per_sample; ++i) {
		data.component_size[i] = plane_info.bits_per_component;
		data.component_pad[i] = plane_info.component_shift[static_cast<size_t>(i)];
	}

	if (frame.format_info.color_family == SourceFrameColorFamily::Rgb) {
		static constexpr int rgb_channels[4] = {
			PL_CHANNEL_R,
			PL_CHANNEL_G,
			PL_CHANNEL_B,
			PL_CHANNEL_A
		};
		for (int i = 0; i < plane_info.components_per_sample; ++i)
			data.component_map[i] = rgb_channels[i];
	}
	else if (plane_index == 0) {
		data.component_map[0] = PL_CHANNEL_Y;
	}
	else if (plane_info.components_per_sample == 2) {
		data.component_map[0] = PL_CHANNEL_CB;
		data.component_map[1] = PL_CHANNEL_CR;
	}
	else if (plane_index == 1) {
		data.component_map[0] = PL_CHANNEL_CB;
	}
	else if (plane_index == 2) {
		data.component_map[0] = PL_CHANNEL_CR;
	}
	else {
		data.component_map[0] = PL_CHANNEL_NONE;
	}

	return true;
}
