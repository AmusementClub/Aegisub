#pragma once

#include "bgra_transform.h"

#ifdef AEGISUB_WITH_HIGHWAY
#include "highway_utils.h"

#include <hwy/highway.h>

namespace aegisub::bgra {
namespace hn = hwy::HWY_NAMESPACE;

/// SIMD-accelerated horizontal flip, byte-for-byte equivalent to the scalar
/// FlipHorizontal in bgra_transform.h. Exposed so benchmarks/tests can opt
/// into the vectorized path; production code still calls FlipHorizontal.
inline void FlipHorizontalSimd(std::vector<unsigned char>& data,
                               int width, int height, ptrdiff_t src_stride) {
	if (width <= 1 || height == 0 || src_stride <= 0)
		return;

	// Treat pixels as uint32 (one BGRA pixel == one lane element). We reverse
	// the whole row as uint32 lanes, leaving the 4 bytes within each pixel
	// untouched (which is exactly what the scalar pixel-swap does).
	const hn::ScalableTag<uint32_t> d;
	const size_t lanes = hn::Lanes(d);

	for (int row = 0; row < height; ++row) {
		auto* px = reinterpret_cast<uint32_t*>(data.data() + static_cast<ptrdiff_t>(row) * src_stride);
		int lo = 0;
		int hi = width - 1;
		// Vectorized middle: mirror full vectors from each end into the other.
		// Process lanes pixels from the left against lanes pixels from the right
		// as long as the two regions don't overlap.
		while (lo + static_cast<int>(lanes) - 1 < hi - static_cast<int>(lanes) + 1) {
			const auto left = hn::LoadU(d, px + lo);
			const auto right = hn::LoadU(d, px + hi - static_cast<int>(lanes) + 1);
			// The right-side chunk was loaded low-index-first; after swapping it
			// must end up reversed at the left position to match a true mirror.
			hn::StoreU(hn::Reverse(d, right), d, px + lo);
			hn::StoreU(hn::Reverse(d, left), d, px + hi - static_cast<int>(lanes) + 1);
			lo += static_cast<int>(lanes);
			hi -= static_cast<int>(lanes);
		}
		// Scalar tail for the remaining center pixels.
		for (; lo < hi; ++lo, --hi)
			std::swap(px[lo], px[hi]);
	}
}

}  // namespace aegisub::bgra

#endif  // AEGISUB_WITH_HIGHWAY
