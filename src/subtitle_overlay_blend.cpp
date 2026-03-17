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

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace {
constexpr SubtitleOverlayRowRange kEmptyRowRange { 0, 0 };

inline unsigned char* RowPointer(BgraSubtitleTargetView target, int y) {
	int physical_y = target.flipped ? (target.height - 1 - y) : y;
	return target.data + static_cast<ptrdiff_t>(physical_y) * target.stride;
}

inline unsigned char const* RowPointer(SubtitleOverlay const& overlay, int y) {
	int physical_y = overlay.flipped ? (overlay.height - 1 - y) : y;
	return overlay.planes[0].data + static_cast<ptrdiff_t>(physical_y) * overlay.planes[0].stride;
}

inline unsigned char const* RowPointer(VideoFrame const& frame, int y) {
	int physical_y = frame.flipped ? (static_cast<int>(frame.height) - 1 - y) : y;
	return frame.data.data() + static_cast<ptrdiff_t>(physical_y) * frame.pitch;
}

inline unsigned char* StorageRowPointer(SubtitleOverlayStorage& storage, int y) {
	int physical_y = storage.flipped ? (storage.height - 1 - y) : y;
	return storage.pixels.data() + static_cast<ptrdiff_t>(physical_y) * storage.pitch;
}

inline unsigned char const* StorageRowPointer(SubtitleOverlayStorage const& storage, int y) {
	int physical_y = storage.flipped ? (storage.height - 1 - y) : y;
	return storage.pixels.data() + static_cast<ptrdiff_t>(physical_y) * storage.pitch;
}

inline unsigned int AssR(std::uint32_t color) { return color >> 24; }
inline unsigned int AssG(std::uint32_t color) { return (color >> 16) & 0xFF; }
inline unsigned int AssB(std::uint32_t color) { return (color >> 8) & 0xFF; }
inline unsigned int AssA(std::uint32_t color) { return color & 0xFF; }

bool FramesAreComparable(VideoFrame const& source, VideoFrame const& composited) {
	if (source.width != composited.width || source.height != composited.height || source.data.empty() || composited.data.empty())
		return false;

	size_t required_row_bytes = source.width * 4;
	size_t required_source_bytes = source.pitch * source.height;
	size_t required_composited_bytes = composited.pitch * composited.height;
	return source.pitch >= required_row_bytes
		&& composited.pitch >= required_row_bytes
		&& source.data.size() >= required_source_bytes
		&& composited.data.size() >= required_composited_bytes;
}

void PrepareSparseOverlayStorage(SubtitleOverlayStorage& storage, int width, int height, bool flipped) {
	bool reusable =
		storage.width == width &&
		storage.height == height &&
		storage.flipped == flipped &&
		storage.pitch == static_cast<size_t>(width) * 4 &&
		storage.pixels.size() == static_cast<size_t>(width) * height * 4 &&
		storage.row_ranges.size() == static_cast<size_t>(height);
	if (!reusable)
		storage.Reset(width, height, flipped);
	else {
		storage.dirty_rects.clear();
		storage.has_visible_content = false;
	}
}

bool PreviousOverlayComparable(SubtitleOverlayStorage const* previous, SubtitleOverlayStorage const& current) {
	return previous &&
		previous->width == current.width &&
		previous->height == current.height &&
		previous->pitch == current.pitch &&
		previous->pixels.size() == current.pixels.size() &&
		previous->row_ranges.size() == current.row_ranges.size();
}

int FirstSetBit4(unsigned int mask) {
	switch (mask & 0xF) {
	case 0x1: case 0x3: case 0x5: case 0x7: case 0x9: case 0xB: case 0xD: case 0xF: return 0;
	case 0x2: case 0x6: case 0xA: case 0xE: return 1;
	case 0x4: case 0xC: return 2;
	case 0x8: return 3;
	default: return 0;
	}
}

int LastSetBit4(unsigned int mask) {
	switch (mask & 0xF) {
	case 0x8: case 0xC: case 0xA: case 0xE: case 0x9: case 0xD: case 0xB: case 0xF: return 3;
	case 0x4: case 0x6: case 0x5: case 0x7: return 2;
	case 0x2: case 0x3: return 1;
	case 0x1: return 0;
	default: return 0;
	}
}

bool RowBandIntersectsActivity(SubtitleOverlayStorage const& storage, int y, int height) {
	return storage.active_row_begin < y + height && storage.active_row_end > y;
}

SubtitleOverlayRowRange ClipRowRange(SubtitleOverlayRowRange const& range, int x0, int x1) {
	if (range.IsEmpty())
		return kEmptyRowRange;

	int clipped_x0 = std::max(range.x0, x0);
	int clipped_x1 = std::min(range.x1, x1);
	if (clipped_x0 >= clipped_x1)
		return kEmptyRowRange;

	return { clipped_x0, clipped_x1 };
}

void MarkDirtyTilesForRow(
	SubtitleOverlayStorage const& current,
	SubtitleOverlayStorage const& previous,
	int y,
	int tile_width,
	int tile_height,
	int tiles_x,
	std::vector<unsigned char>& dirty_tiles) {
	auto current_range = current.row_ranges[static_cast<size_t>(y)];
	auto previous_range = previous.row_ranges[static_cast<size_t>(y)];
	if (current_range.IsEmpty() && previous_range.IsEmpty())
		return;

	int min_x = current.width;
	int max_x = 0;
	if (!current_range.IsEmpty()) {
		min_x = std::min(min_x, current_range.x0);
		max_x = std::max(max_x, current_range.x1);
	}
	if (!previous_range.IsEmpty()) {
		min_x = std::min(min_x, previous_range.x0);
		max_x = std::max(max_x, previous_range.x1);
	}
	if (min_x >= max_x)
		return;

	int tile_y = y / tile_height;
	int tile_begin = min_x / tile_width;
	int tile_end = (max_x - 1) / tile_width;
	for (int tile_x = tile_begin; tile_x <= tile_end; ++tile_x) {
		int rect_x0 = tile_x * tile_width;
		int rect_x1 = std::min(current.width, rect_x0 + tile_width);
		auto clipped_current = ClipRowRange(current_range, rect_x0, rect_x1);
		auto clipped_previous = ClipRowRange(previous_range, rect_x0, rect_x1);
		if (clipped_current.IsEmpty() && clipped_previous.IsEmpty())
			continue;

		if (clipped_current.x0 != clipped_previous.x0 || clipped_current.x1 != clipped_previous.x1) {
			dirty_tiles[static_cast<size_t>(tile_y) * tiles_x + tile_x] = 1;
			continue;
		}

		auto const* current_row = StorageRowPointer(current, y) + static_cast<ptrdiff_t>(clipped_current.x0) * 4;
		auto const* previous_row = StorageRowPointer(previous, y) + static_cast<ptrdiff_t>(clipped_previous.x0) * 4;
		if (std::memcmp(current_row, previous_row, static_cast<size_t>(clipped_current.x1 - clipped_current.x0) * 4) != 0)
			dirty_tiles[static_cast<size_t>(tile_y) * tiles_x + tile_x] = 1;
	}
}

bool BuildDirtyRectsFromTileMask(SubtitleOverlayStorage& current, std::vector<unsigned char> const& dirty_tiles, int tile_width, int tile_height) {
	current.dirty_rects.clear();
	if (current.width <= 0 || current.height <= 0 || tile_width <= 0 || tile_height <= 0)
		return false;

	int tiles_x = (current.width + tile_width - 1) / tile_width;
	int tiles_y = (current.height + tile_height - 1) / tile_height;
	std::vector<SubtitleOverlayDirtyRect> merged_rects;

	for (int tile_y = 0; tile_y < tiles_y; ++tile_y) {
		int y = tile_y * tile_height;
		int rect_height = std::min(tile_height, current.height - y);
		int run_start_x = -1;
		int run_width = 0;

		auto flush_run = [&]() {
			if (run_start_x < 0)
				return;

			if (!merged_rects.empty()) {
				auto& prev_rect = merged_rects.back();
				if (prev_rect.x == run_start_x
					&& prev_rect.width == run_width
					&& prev_rect.y + prev_rect.height == y) {
					prev_rect.height += rect_height;
					run_start_x = -1;
					run_width = 0;
					return;
				}
			}

			merged_rects.push_back({ run_start_x, y, run_width, rect_height });
			run_start_x = -1;
			run_width = 0;
		};

		for (int tile_x = 0; tile_x < tiles_x; ++tile_x) {
			if (!dirty_tiles[static_cast<size_t>(tile_y) * tiles_x + tile_x]) {
				flush_run();
				continue;
			}

			int rect_x = tile_x * tile_width;
			int rect_width = std::min(tile_width, current.width - rect_x);
			if (run_start_x < 0)
				run_start_x = rect_x;
			run_width += rect_width;
		}

		flush_run();
	}

	current.dirty_rects = std::move(merged_rects);
	return !current.dirty_rects.empty();
}

bool BuildSparsePremultipliedCompatibilityOverlayScalar(VideoFrame const& source, VideoFrame const& composited, SubtitleOverlayStorage& storage) {
	int width = static_cast<int>(source.width);
	int height = static_cast<int>(source.height);
	size_t row_bytes = static_cast<size_t>(width) * 4;
	storage.active_row_begin = height;
	storage.active_row_end = 0;

	for (int y = 0; y < height; ++y) {
		auto const* src_row = RowPointer(source, y);
		auto const* composited_row = RowPointer(composited, y);
		auto previous_range = storage.row_ranges[static_cast<size_t>(y)];
		auto* dst_row = StorageRowPointer(storage, y);

		if (std::memcmp(src_row, composited_row, row_bytes) == 0) {
			if (!previous_range.IsEmpty())
				std::memset(dst_row, 0, row_bytes);
			storage.row_ranges[static_cast<size_t>(y)] = kEmptyRowRange;
			continue;
		}

		if (!previous_range.IsEmpty())
			std::memset(dst_row, 0, row_bytes);

		int row_x0 = width;
		int row_x1 = 0;
		for (int x = 0; x < width; ++x) {
			auto pixel_offset = static_cast<ptrdiff_t>(x) * 4;
			auto const* src = src_row + pixel_offset;
			auto const* composited_pixel = composited_row + pixel_offset;
			auto* dst = dst_row + pixel_offset;
			if (std::memcmp(src, composited_pixel, 4) != 0) {
				dst[0] = composited_pixel[0];
				dst[1] = composited_pixel[1];
				dst[2] = composited_pixel[2];
				dst[3] = 255;
				row_x0 = std::min(row_x0, x);
				row_x1 = x + 1;
				storage.has_visible_content = true;
			}
		}

		if (row_x0 < row_x1) {
			storage.row_ranges[static_cast<size_t>(y)] = { row_x0, row_x1 };
			storage.active_row_begin = std::min(storage.active_row_begin, y);
			storage.active_row_end = y + 1;
		}
		else {
			storage.row_ranges[static_cast<size_t>(y)] = kEmptyRowRange;
		}
	}

	return true;
}

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
bool BuildSparsePremultipliedCompatibilityOverlaySimd(
	VideoFrame const& source,
	VideoFrame const& composited,
	SubtitleOverlayStorage& storage,
	SubtitleOverlayStorage const* previous = nullptr,
	int tile_width = 0,
	int tile_height = 0,
	std::vector<unsigned char>* dirty_tiles = nullptr) {
	int width = static_cast<int>(source.width);
	int height = static_cast<int>(source.height);
	size_t row_bytes = static_cast<size_t>(width) * 4;
	__m128i const all_ones = _mm_set1_epi32(-1);
	__m128i const alpha_mask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
	int tiles_x = (tile_width > 0) ? (width + tile_width - 1) / tile_width : 0;
	storage.active_row_begin = height;
	storage.active_row_end = 0;

	for (int y = 0; y < height; ++y) {
		auto const* src_row = RowPointer(source, y);
		auto const* composited_row = RowPointer(composited, y);
		auto previous_range = storage.row_ranges[static_cast<size_t>(y)];
		auto* dst_row = StorageRowPointer(storage, y);

		if (std::memcmp(src_row, composited_row, row_bytes) == 0) {
			if (!previous_range.IsEmpty())
				std::memset(dst_row, 0, row_bytes);
			storage.row_ranges[static_cast<size_t>(y)] = kEmptyRowRange;
			continue;
		}

		if (!previous_range.IsEmpty())
			std::memset(dst_row, 0, row_bytes);

		int row_x0 = width;
		int row_x1 = 0;
		int simd_width = width & ~3;
		int x = 0;
		for (; x < simd_width; x += 4) {
			auto const* src = src_row + static_cast<ptrdiff_t>(x) * 4;
			auto const* composited_pixel = composited_row + static_cast<ptrdiff_t>(x) * 4;
			__m128i src_vec = _mm_loadu_si128(reinterpret_cast<__m128i const*>(src));
			__m128i composited_vec = _mm_loadu_si128(reinterpret_cast<__m128i const*>(composited_pixel));
			__m128i equal_mask = _mm_cmpeq_epi32(src_vec, composited_vec);
			if (_mm_movemask_epi8(equal_mask) == 0xFFFF)
				continue;

			unsigned int changed_lanes = static_cast<unsigned int>(~_mm_movemask_ps(_mm_castsi128_ps(equal_mask))) & 0xF;
			__m128i changed_mask = _mm_xor_si128(equal_mask, all_ones);
			__m128i with_alpha = _mm_or_si128(composited_vec, alpha_mask);
			__m128i sparse = _mm_and_si128(changed_mask, with_alpha);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(dst_row + static_cast<ptrdiff_t>(x) * 4), sparse);
			row_x0 = std::min(row_x0, x + FirstSetBit4(changed_lanes));
			row_x1 = std::max(row_x1, x + LastSetBit4(changed_lanes) + 1);
			storage.has_visible_content = true;
		}

		for (; x < width; ++x) {
			auto pixel_offset = static_cast<ptrdiff_t>(x) * 4;
			auto const* src = src_row + pixel_offset;
			auto const* composited_pixel = composited_row + pixel_offset;
			auto* dst = dst_row + pixel_offset;
			if (std::memcmp(src, composited_pixel, 4) != 0) {
				dst[0] = composited_pixel[0];
				dst[1] = composited_pixel[1];
				dst[2] = composited_pixel[2];
				dst[3] = 255;
				row_x0 = std::min(row_x0, x);
				row_x1 = x + 1;
				storage.has_visible_content = true;
			}
		}

		if (row_x0 < row_x1) {
			storage.row_ranges[static_cast<size_t>(y)] = { row_x0, row_x1 };
			storage.active_row_begin = std::min(storage.active_row_begin, y);
			storage.active_row_end = y + 1;
		}
		else {
			storage.row_ranges[static_cast<size_t>(y)] = kEmptyRowRange;
		}

		if (previous && dirty_tiles && tile_width > 0 && tile_height > 0)
			MarkDirtyTilesForRow(storage, *previous, y, tile_width, tile_height, tiles_x, *dirty_tiles);
	}

	return true;
}
#endif

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

bool BuildSparsePremultipliedCompatibilityOverlay(VideoFrame const& source, VideoFrame const& composited, SubtitleOverlayStorage& storage, SubtitleOverlay& overlay) {
	if (!FramesAreComparable(source, composited)) {
		overlay = { };
		return false;
	}

	int width = static_cast<int>(source.width);
	int height = static_cast<int>(source.height);
	PrepareSparseOverlayStorage(storage, width, height, source.flipped);

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
	BuildSparsePremultipliedCompatibilityOverlaySimd(source, composited, storage);
#else
	BuildSparsePremultipliedCompatibilityOverlayScalar(source, composited, storage);
#endif

	overlay = storage.MakeView(true);
	overlay.color_role = SubtitleOverlayColorRole::SubtitleVideoCompatibility;
	return true;
}

bool BuildSparsePremultipliedCompatibilityOverlayWithDirtyTiles(
	VideoFrame const& source,
	VideoFrame const& composited,
	SubtitleOverlayStorage const* previous,
	SubtitleOverlayStorage& storage,
	SubtitleOverlay& overlay,
	int tile_width,
	int tile_height) {
	if (!FramesAreComparable(source, composited) || tile_width <= 0 || tile_height <= 0) {
		overlay = { };
		return false;
	}

	int width = static_cast<int>(source.width);
	int height = static_cast<int>(source.height);
	PrepareSparseOverlayStorage(storage, width, height, source.flipped);

	auto const has_previous = PreviousOverlayComparable(previous, storage);
	if (!has_previous) {
		if (!BuildSparsePremultipliedCompatibilityOverlay(source, composited, storage, overlay))
			return false;
		BuildDirtyTileRectsForOverlay(previous, storage, tile_width, tile_height);
		overlay = storage.MakeView(true);
		overlay.color_role = SubtitleOverlayColorRole::SubtitleVideoCompatibility;
		return true;
	}

	int tiles_x = (width + tile_width - 1) / tile_width;
	int tiles_y = (height + tile_height - 1) / tile_height;
	std::vector<unsigned char> dirty_tiles(static_cast<size_t>(tiles_x) * tiles_y, 0);

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
	BuildSparsePremultipliedCompatibilityOverlaySimd(source, composited, storage, previous, tile_width, tile_height, &dirty_tiles);
#else
	BuildSparsePremultipliedCompatibilityOverlayScalar(source, composited, storage);
	for (int y = 0; y < height; ++y)
		MarkDirtyTilesForRow(storage, *previous, y, tile_width, tile_height, tiles_x, dirty_tiles);
#endif

	BuildDirtyRectsFromTileMask(storage, dirty_tiles, tile_width, tile_height);
	overlay = storage.MakeView(true);
	overlay.color_role = SubtitleOverlayColorRole::SubtitleVideoCompatibility;
	return true;
}

bool BuildDirtyTileRectsForOverlay(SubtitleOverlayStorage const* previous, SubtitleOverlayStorage& current, int tile_width, int tile_height) {
	current.dirty_rects.clear();
	if (current.width <= 0 || current.height <= 0 || current.pitch == 0 || tile_width <= 0 || tile_height <= 0)
		return false;

	auto const has_previous = PreviousOverlayComparable(previous, current);

	std::vector<SubtitleOverlayDirtyRect> merged_rects;

	for (int y = 0; y < current.height; y += tile_height) {
		int rect_height = std::min(tile_height, current.height - y);
		if (has_previous && !RowBandIntersectsActivity(current, y, rect_height) && !RowBandIntersectsActivity(*previous, y, rect_height))
			continue;

		int run_start_x = -1;
		int run_width = 0;

		auto flush_run = [&]() {
			if (run_start_x < 0)
				return;

			if (!merged_rects.empty()) {
				auto& prev_rect = merged_rects.back();
				if (prev_rect.x == run_start_x
					&& prev_rect.width == run_width
					&& prev_rect.y + prev_rect.height == y) {
					prev_rect.height += rect_height;
					run_start_x = -1;
					run_width = 0;
					return;
				}
			}

			merged_rects.push_back({ run_start_x, y, run_width, rect_height });
			run_start_x = -1;
			run_width = 0;
		};

		for (int x = 0; x < current.width; x += tile_width) {
			int rect_width = std::min(tile_width, current.width - x);
			bool dirty = !has_previous;

			if (has_previous) {
				int rect_x1 = x + rect_width;
				for (int row = 0; row < rect_height && !dirty; ++row) {
					auto current_range = ClipRowRange(current.row_ranges[static_cast<size_t>(y + row)], x, rect_x1);
					auto previous_range = ClipRowRange(previous->row_ranges[static_cast<size_t>(y + row)], x, rect_x1);
					if (current_range.IsEmpty() && previous_range.IsEmpty())
						continue;
					if (current_range.x0 != previous_range.x0 || current_range.x1 != previous_range.x1) {
						dirty = true;
						break;
					}

					auto const* current_row = StorageRowPointer(current, y + row) + static_cast<ptrdiff_t>(current_range.x0) * 4;
					auto const* previous_row = StorageRowPointer(*previous, y + row) + static_cast<ptrdiff_t>(previous_range.x0) * 4;
					if (std::memcmp(current_row, previous_row, static_cast<size_t>(current_range.x1 - current_range.x0) * 4) != 0)
						dirty = true;
				}
			}

			if (!dirty) {
				flush_run();
				continue;
			}

			if (run_start_x < 0)
				run_start_x = x;
			run_width += rect_width;
		}

		flush_run();
	}

	current.dirty_rects = std::move(merged_rects);
	return !current.dirty_rects.empty();
}

bool ExtractOpaqueBgraDifferenceOverlay(VideoFrame const& source, VideoFrame const& composited, SubtitleOverlayStorage& storage, SubtitleOverlay& overlay) {
	if (!FramesAreComparable(source, composited)) {
		overlay = { };
		return false;
	}

	int width = static_cast<int>(source.width);
	int height = static_cast<int>(source.height);
	int min_x = width;
	int min_y = height;
	int max_x = -1;
	int max_y = -1;

	for (int y = 0; y < height; ++y) {
		auto const* src_row = RowPointer(source, y);
		auto const* composited_row = RowPointer(composited, y);
		if (std::memcmp(src_row, composited_row, static_cast<size_t>(width) * 4) == 0)
			continue;

		min_y = std::min(min_y, y);
		max_y = y;
		for (int x = 0; x < width; ++x) {
			auto pixel_offset = static_cast<ptrdiff_t>(x) * 4;
			if (std::memcmp(src_row + pixel_offset, composited_row + pixel_offset, 4) != 0) {
				min_x = std::min(min_x, x);
				max_x = std::max(max_x, x);
			}
		}
	}

	if (max_x < min_x || max_y < min_y) {
		overlay = { };
		return false;
	}

	int patch_width = max_x - min_x + 1;
	int patch_height = max_y - min_y + 1;
	storage.Reset(patch_width, patch_height, source.flipped);

	for (int y = 0; y < patch_height; ++y) {
		auto const* src_row = RowPointer(composited, min_y + y) + static_cast<ptrdiff_t>(min_x) * 4;
		auto* dst_row = StorageRowPointer(storage, y);
		std::memcpy(dst_row, src_row, static_cast<size_t>(patch_width) * 4);
	}

	overlay = storage.MakeView(false);
	overlay.canvas_width = width;
	overlay.canvas_height = height;
	overlay.target_x = min_x;
	overlay.target_y = source.flipped ? (height - max_y - 1) : min_y;
	overlay.color_role = SubtitleOverlayColorRole::SubtitleVideoCompatibility;
	overlay.composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;
	return true;
}

void CompositeOpaqueBgraOverlayOntoVideoFrame(VideoFrame& frame, SubtitleOverlay const& overlay) {
	if (!overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8 || overlay.composition_mode != SubtitleOverlayCompositionMode::OpaqueReplace)
		return;

	int x0 = std::max(0, overlay.target_x);
	int y0 = std::max(0, overlay.target_y);
	int x1 = std::min(static_cast<int>(frame.width), overlay.target_x + overlay.width);
	int y1 = std::min(static_cast<int>(frame.height), overlay.target_y + overlay.height);
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int y = y0; y < y1; ++y) {
		int overlay_y = y - overlay.target_y;
		int dst_y = frame.flipped ? (static_cast<int>(frame.height) - 1 - y) : y;
		auto* dst_row = frame.data.data() + static_cast<ptrdiff_t>(dst_y) * frame.pitch + static_cast<ptrdiff_t>(x0) * 4;
		auto const* src_row = RowPointer(overlay, overlay_y) + static_cast<ptrdiff_t>(x0 - overlay.target_x) * 4;
		std::memcpy(dst_row, src_row, static_cast<size_t>(x1 - x0) * 4);
	}
}

void CompositePremultipliedBgraOverlayOntoVideoFrame(VideoFrame& frame, SubtitleOverlay const& overlay) {
	if (!overlay.IsValid() || !overlay.premultiplied_alpha || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return;

	int x0 = std::max(0, overlay.target_x);
	int y0 = std::max(0, overlay.target_y);
	int x1 = std::min(static_cast<int>(frame.width), overlay.target_x + overlay.width);
	int y1 = std::min(static_cast<int>(frame.height), overlay.target_y + overlay.height);
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int y = y0; y < y1; ++y) {
		int overlay_y = y - overlay.target_y;
		int dst_y = frame.flipped ? (static_cast<int>(frame.height) - 1 - y) : y;
		auto* dst_row = frame.data.data() + static_cast<ptrdiff_t>(dst_y) * frame.pitch;
		auto const* src_row = RowPointer(overlay, overlay_y);

		for (int x = x0; x < x1; ++x) {
			auto const* src = src_row + static_cast<ptrdiff_t>(x - overlay.target_x) * 4;
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
