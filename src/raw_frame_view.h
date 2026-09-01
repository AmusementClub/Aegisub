#pragma once

// Non-owning views over raw BGRA video frames, plus the two pixel-level
// mappings every consumer of those views needs: the flipped-row mapping and
// the gray formula.
//
// These started inside motion_track/gray_convert.h, but nothing here is
// specific to motion tracking -- async_video_provider, the overlay blend and
// the outline probe all need the same views and the same row order. Keeping
// one definition is what stops a converted interior and a border fill
// computed elsewhere from drifting apart. motion_track re-exports these names
// so its own call sites stay unqualified.

#include <cstdint>

namespace aegisub {

// View over any caller-supplied BGRA buffer, 4 bytes per pixel.
struct BgraView {
	std::uint8_t const* data = nullptr;
	int stride = 0; // bytes per row, >= width*4
	int width = 0;
	int height = 0;
	bool flipped = false;
};

// View of one raw BGRA video frame owned by the video provider. Layout is
// identical to BgraView; the separate type carries the lifetime contract --
// storage stays valid only until the next fetch or the end of the batch
// callback, so it must never be stored past that point.
struct RawBgraView {
	std::uint8_t const* data = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0; // bytes per row
	bool flipped = false;
};

// Borrows a provider frame as a plain BgraView. Does not extend the frame's
// lifetime: the result dies with the RawBgraView it came from.
constexpr BgraView AsBgraView(RawBgraView const& view) {
	return BgraView{view.data, view.pitch, view.width, view.height, view.flipped};
}

// Physical row that stores logical row logical_y in a frame of the given
// height: a vertically flipped BGRA source stores row 0 last. The single
// definition of the flipped-row mapping -- BgraToGray, the raw batch reader
// and NormalizeFrameY all route through it so their row orders can never
// drift apart. Pure mapping: callers keep their own bounds checks.
constexpr int LogicalRowToPhysicalRow(int logical_y, int height, bool flipped) {
	return flipped ? height - 1 - logical_y : logical_y;
}

// Gray value of one BGRA pixel.
// Formula: Y = (77*R + 150*G + 29*B) >> 8 (BT.601, integer exact).
// The single definition of the formula: every path that grays a pixel goes
// through here.
inline std::uint8_t BgraPixelToGray(std::uint8_t const* px) {
	// BGRA byte order: [0]=B, [1]=G, [2]=R.
	return static_cast<std::uint8_t>(
		(77u * px[2] + 150u * px[1] + 29u * px[0]) >> 8);
}

} // namespace aegisub
