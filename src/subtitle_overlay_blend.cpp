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

#include "subtitle_overlay_blend.h"

#include <algorithm>
#include <cstring>

namespace {
inline unsigned char* RowPointer(BgraSubtitleTargetView target, int y) {
	int physical_y = target.flipped ? (target.height - 1 - y) : y;
	return target.data + static_cast<ptrdiff_t>(physical_y) * target.stride;
}

inline unsigned char const* RowPointer(SubtitleOverlay const& overlay, int y) {
	int physical_y = overlay.flipped ? (overlay.height - 1 - y) : y;
	return overlay.planes[0].data + static_cast<ptrdiff_t>(physical_y) * overlay.planes[0].stride;
}

inline unsigned int AssR(std::uint32_t color) { return color >> 24; }
inline unsigned int AssG(std::uint32_t color) { return (color >> 16) & 0xFF; }
inline unsigned int AssB(std::uint32_t color) { return (color >> 8) & 0xFF; }
inline unsigned int AssA(std::uint32_t color) { return color & 0xFF; }
}

void ClearBgraSubtitleTarget(BgraSubtitleTargetView target) {
	if (!target.data || target.width <= 0 || target.height <= 0 || target.stride == 0)
		return;

	for (int y = 0; y < target.height; ++y)
		std::memset(RowPointer(target, y), 0, static_cast<size_t>(target.width) * 4);
}

void BlendLibassMaskIntoBgraTarget(
	BgraSubtitleTargetView target,
	SubtitleOverlayBlendMode mode,
	int dst_x,
	int dst_y,
	int mask_w,
	int mask_h,
	unsigned char const* mask_data,
	ptrdiff_t mask_stride,
	std::uint32_t ass_color) {
	if (!target.data || !mask_data || target.width <= 0 || target.height <= 0 || mask_w <= 0 || mask_h <= 0)
		return;

	int x0 = std::max(0, dst_x);
	int y0 = std::max(0, dst_y);
	int x1 = std::min(target.width, dst_x + mask_w);
	int y1 = std::min(target.height, dst_y + mask_h);
	if (x0 >= x1 || y0 >= y1)
		return;

	unsigned int opacity = 255 - AssA(ass_color);
	unsigned int r = AssR(ass_color);
	unsigned int g = AssG(ass_color);
	unsigned int b = AssB(ass_color);

	for (int y = y0; y < y1; ++y) {
		auto* dst_row = RowPointer(target, y);
		auto const* src_row = mask_data + static_cast<ptrdiff_t>(y - dst_y) * mask_stride;

		for (int x = x0; x < x1; ++x) {
			unsigned int src_alpha = static_cast<unsigned int>(src_row[x - dst_x]) * opacity / 255;
			if (!src_alpha)
				continue;

			auto* dst = dst_row + static_cast<ptrdiff_t>(x) * 4;
			if (mode == SubtitleOverlayBlendMode::LegacyBakeIn) {
				unsigned int inv_alpha = 255 - src_alpha;
				dst[0] = static_cast<unsigned char>((src_alpha * b + inv_alpha * dst[0]) / 255);
				dst[1] = static_cast<unsigned char>((src_alpha * g + inv_alpha * dst[1]) / 255);
				dst[2] = static_cast<unsigned char>((src_alpha * r + inv_alpha * dst[2]) / 255);
				dst[3] = 0;
			}
			else {
				unsigned int inv_alpha = 255 - src_alpha;
				unsigned int src_b = src_alpha * b / 255;
				unsigned int src_g = src_alpha * g / 255;
				unsigned int src_r = src_alpha * r / 255;
				dst[0] = static_cast<unsigned char>(src_b + dst[0] * inv_alpha / 255);
				dst[1] = static_cast<unsigned char>(src_g + dst[1] * inv_alpha / 255);
				dst[2] = static_cast<unsigned char>(src_r + dst[2] * inv_alpha / 255);
				dst[3] = static_cast<unsigned char>(src_alpha + dst[3] * inv_alpha / 255);
			}
		}
	}
}

void CompositePremultipliedBgraOverlayOntoVideoFrame(VideoFrame& frame, SubtitleOverlay const& overlay) {
	if (!overlay.IsValid() || !overlay.premultiplied_alpha || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return;

	int width = std::min(static_cast<int>(frame.width), overlay.width);
	int height = std::min(static_cast<int>(frame.height), overlay.height);
	if (width <= 0 || height <= 0)
		return;

	for (int y = 0; y < height; ++y) {
		int dst_y = frame.flipped ? (height - 1 - y) : y;
		auto* dst_row = frame.data.data() + static_cast<ptrdiff_t>(dst_y) * frame.pitch;
		auto const* src_row = RowPointer(overlay, y);

		for (int x = 0; x < width; ++x) {
			auto const* src = src_row + static_cast<ptrdiff_t>(x) * 4;
			auto* dst = dst_row + static_cast<ptrdiff_t>(x) * 4;
			unsigned int src_alpha = src[3];
			if (!src_alpha)
				continue;

			unsigned int inv_alpha = 255 - src_alpha;
			dst[0] = static_cast<unsigned char>(src[0] + dst[0] * inv_alpha / 255);
			dst[1] = static_cast<unsigned char>(src[1] + dst[1] * inv_alpha / 255);
			dst[2] = static_cast<unsigned char>(src[2] + dst[2] * inv_alpha / 255);
			dst[3] = 0;
		}
	}
}
