#include "gray_convert.h"

#include <algorithm>

namespace aegisub::motion_track {

bool BgraToGray(BgraView const& src, GrayPatch& out) {
	if (!src.data || src.width <= 0 || src.height <= 0
	    || src.stride < src.width * 4)
		return false;

	int const width = src.width;
	int const height = src.height;
	out.origin_x = 0;
	out.origin_y = 0;
	out.width = width;
	out.height = height;
	out.stride = width;
	out.gray.resize(size_t(width) * size_t(height));

	for (int y = 0; y < height; ++y) {
		int const physical_y = LogicalRowToPhysicalRow(y, height, src.flipped);
		auto const* row = src.data
		                + int64_t(physical_y) * src.stride;
		auto* dst = out.gray.data() + int64_t(y) * width;
		for (int x = 0; x < width; ++x)
			dst[x] = BgraPixelToGray(row + size_t(x) * 4);
	}
	return true;
}

} // namespace aegisub::motion_track
