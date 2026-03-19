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

#include "video_renderer_opengl_tile.h"

#include <cmath>

namespace {
int SmallestPowerOf2Local(int x) {
	if (x <= 1)
		return 1;

	--x;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	return x + 1;
}
}

OpenGLVideoRendererTileLayout BuildOpenGLVideoRendererTileLayout(int frame_width, int frame_height, int bytes_per_pixel, int max_texture_size, bool supports_rectangular_textures, bool flipped) {
	OpenGLVideoRendererTileLayout layout;
	layout.frame_width = frame_width;
	layout.frame_height = frame_height;
	layout.flipped = flipped;

	if (frame_width <= 0 || frame_height <= 0 || bytes_per_pixel <= 0 || max_texture_size <= 0)
		return layout;

	int texture_area = std::max(max_texture_size - 2, 1);
	layout.texture_rows = static_cast<int>(std::ceil(static_cast<double>(frame_height) / texture_area));
	layout.texture_cols = static_cast<int>(std::ceil(static_cast<double>(frame_width) / texture_area));
	layout.tiles.reserve(layout.texture_rows * layout.texture_cols);

	int last_row = layout.texture_rows - 1;
	int last_col = layout.texture_cols - 1;
	for (int row = 0; row < layout.texture_rows; ++row) {
		for (int col = 0; col < layout.texture_cols; ++col) {
			OpenGLVideoRendererTile tile;
			tile.source_x = col * texture_area;
			tile.source_y = row * texture_area;
			tile.source_w = std::min(frame_width - tile.source_x, max_texture_size);
			tile.source_h = std::min(frame_height - tile.source_y, max_texture_size);
			tile.data_offset = tile.source_y * frame_width * bytes_per_pixel + tile.source_x * bytes_per_pixel;

			int texture_height = SmallestPowerOf2Local(tile.source_h);
			int texture_width = SmallestPowerOf2Local(tile.source_w);
			if (!supports_rectangular_textures)
				texture_width = texture_height = std::max(texture_width, texture_height);

			tile.texture_w = texture_width;
			tile.texture_h = texture_height;
			tile.x1 = static_cast<float>(tile.source_x + (col != 0));
			tile.y1 = static_cast<float>(tile.source_y + (row != 0));
			tile.x2 = static_cast<float>(tile.source_x + tile.source_w - (col != last_col));
			tile.y2 = static_cast<float>(tile.source_y + tile.source_h - (row != last_row));
			tile.u1 = col == 0 ? 0.0f : 1.0f / texture_width;
			tile.v1 = row == 0 ? 0.0f : 1.0f / texture_height;
			tile.u2 = static_cast<float>(col == last_col ? tile.source_w : tile.source_w - 1) / texture_width;
			tile.v2 = static_cast<float>(row == last_row ? tile.source_h : tile.source_h - 1) / texture_height;
			layout.tiles.emplace_back(tile);
		}
	}

	return layout;
}

std::array<float, 16> BuildOpenGLVideoRendererOrthoMatrix(int width, int height, bool flipped) {
	std::array<float, 16> matrix = { };
	if (width <= 0 || height <= 0)
		return matrix;

	matrix[0] = 2.0f / width;
	matrix[5] = flipped ? 2.0f / height : -2.0f / height;
	matrix[10] = 1.0f;
	matrix[12] = -1.0f;
	matrix[13] = flipped ? -1.0f : 1.0f;
	matrix[15] = 1.0f;
	return matrix;
}
