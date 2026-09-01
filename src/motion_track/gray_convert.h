#pragma once

// BGRA -> gray patch conversion. The views, the flipped-row mapping and the
// gray formula itself live in src/raw_frame_view.h (nothing about them is
// motion-tracking specific); they are re-exported below so call sites inside
// this namespace stay unqualified.
// Scalar implementation is the byte-exact reference; a Highway row kernel
// may replace the loop later only if it stays byte-equivalent.

#include "types.h"

#include "../raw_frame_view.h"

#include <cstdint>

namespace aegisub::motion_track {

using aegisub::AsBgraView;
using aegisub::BgraPixelToGray;
using aegisub::BgraView;
using aegisub::LogicalRowToPhysicalRow;

// Converts src into out: out.frame preserved, origin set to (0, 0),
// stride == width, gray resized. Returns false (out untouched) when src is
// degenerate (null data, non-positive dims, stride < width*4).
bool BgraToGray(BgraView const& src, GrayPatch& out);

} // namespace aegisub::motion_track
