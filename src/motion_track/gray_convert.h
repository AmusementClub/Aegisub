#pragma once

// Formula: Y = (77*R + 150*G + 29*B) >> 8 (BT.601, integer exact).
// Row order honors a flipped source via LogicalRowToPhysicalRow below, the
// same mapping NormalizeFrameY applies to keypoint rows.
// Scalar implementation is the byte-exact reference; a Highway row kernel
// may replace the loop later only if it stays byte-equivalent.

#include "types.h"

#include <cstdint>

namespace aegisub::motion_track {

struct BgraView {
	std::uint8_t const* data = nullptr; // BGRA, 4 bytes per pixel
	int stride = 0;                     // bytes per row, >= width*4
	int width = 0;
	int height = 0;
	bool flipped = false;
};

// Physical row that stores logical row logical_y in a frame of the given
// height: a vertically flipped BGRA source stores row 0 last. The single
// definition of the flipped-row mapping — BgraToGray, the raw batch reader
// and NormalizeFrameY all route through it so their row orders can never
// drift apart. Pure mapping: callers keep their own bounds checks.
constexpr int LogicalRowToPhysicalRow(int logical_y, int height, bool flipped) {
	return flipped ? height - 1 - logical_y : logical_y;
}

// Gray value of one BGRA pixel. The single definition of the formula: every
// path that grays a pixel goes through here so a converted interior and a
// border fill computed elsewhere can never drift apart.
inline std::uint8_t BgraPixelToGray(std::uint8_t const* px) {
	// BGRA byte order: [0]=B, [1]=G, [2]=R.
	return static_cast<std::uint8_t>(
		(77u * px[2] + 150u * px[1] + 29u * px[0]) >> 8);
}

// Converts src into out: out.frame preserved, origin set to (0, 0),
// stride == width, gray resized. Returns false (out untouched) when src is
// degenerate (null data, non-positive dims, stride < width*4).
bool BgraToGray(BgraView const& src, GrayPatch& out);

} // namespace aegisub::motion_track
