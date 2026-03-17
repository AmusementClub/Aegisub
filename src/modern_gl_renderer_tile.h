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

#include <array>
#include <vector>

struct ModernGLTile {
	int data_offset = 0;
	int source_x = 0;
	int source_y = 0;
	int source_w = 0;
	int source_h = 0;
	int texture_w = 0;
	int texture_h = 0;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float x2 = 0.0f;
	float y2 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
	float u2 = 0.0f;
	float v2 = 0.0f;
};

struct ModernGLTileLayout {
	int frame_width = 0;
	int frame_height = 0;
	int texture_rows = 0;
	int texture_cols = 0;
	bool flipped = false;
	std::vector<ModernGLTile> tiles;
};

ModernGLTileLayout BuildModernGLTileLayout(int frame_width, int frame_height, int bytes_per_pixel, int max_texture_size, bool supports_rectangular_textures, bool flipped);
std::array<float, 16> BuildModernGLOrthoMatrix(int width, int height, bool flipped);
