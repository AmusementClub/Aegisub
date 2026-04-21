// Copyright (c) 2026
// All rights reserved.

#pragma once

#include <cstdint>

/// A simple axis-aligned rectangle for the audio display render model.
/// Replaces wxRect in platform-agnostic render model data structures.
struct AudioDisplayRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

/// A pen style for the audio display render model.
/// Replaces wxPen in platform-agnostic render model data structures.
struct AudioDisplayPenStyle {
	uint32_t colour = 0xFF000000u;
	int width = 1;
};

/// Pack RGBA components into a single uint32_t in BGRA byte order.
/// This matches kBGRA_8888_SkColorType and the existing PackBgra() helper.
inline uint32_t AudioDisplayPackColour(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
	return static_cast<uint32_t>(b)
		| (static_cast<uint32_t>(g) << 8)
		| (static_cast<uint32_t>(r) << 16)
		| (static_cast<uint32_t>(a) << 24);
}

inline uint8_t AudioDisplayColourR(uint32_t c) { return static_cast<uint8_t>((c >> 16) & 0xFF); }
inline uint8_t AudioDisplayColourG(uint32_t c) { return static_cast<uint8_t>((c >> 8) & 0xFF); }
inline uint8_t AudioDisplayColourB(uint32_t c) { return static_cast<uint8_t>(c & 0xFF); }
inline uint8_t AudioDisplayColourA(uint32_t c) { return static_cast<uint8_t>((c >> 24) & 0xFF); }
