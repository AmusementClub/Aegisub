#include "raw_batch_motion_frame_reader.h"

#include <algorithm>

namespace aegisub::motion_track {

FrameReadResult RawBatchMotionFrameReader::FetchGray(
	int frame, RoiRect roi, GrayPatch& out) {
	RawBgraView view;
	auto result = access.FetchBgra(frame, view);
	if (result.status != FrameReadStatus::Ok)
		return result;
	if (!view.data || view.width <= 0 || view.height <= 0 || static_cast<int64_t>(view.pitch) < int64_t(view.width) * 4)
		return {FrameReadStatus::FrameUnavailable, "invalid frame buffer"};

	int const w = std::max(roi.w, 0);
	int const h = std::max(roi.h, 0);

	// Mean of the in-bounds intersection for border fill; neutral 128 when
	// the ROI misses the frame entirely.
	long long sum = 0;
	long long count = 0;
	for (int y = std::max(roi.y, 0); y < std::min(roi.y + h, view.height); ++y) {
		// Same logical -> physical row mapping as the copy loop below: a
		// flipped view stores row 0 last, so sampling y directly would average
		// a different band of the frame than the one actually copied.
		int const physical_y = LogicalRowToPhysicalRow(y, view.height, view.flipped);
		auto const *row = view.data + int64_t(physical_y) * view.pitch;
		for (int x = std::max(roi.x, 0); x < std::min(roi.x + w, view.width); ++x) {
			// Same helper the copy loop uses, so filled borders and
			// converted interiors can never drift apart.
			sum += BgraPixelToGray(row + size_t(x) * 4);
			++count;
		}
	}
	std::uint8_t const fill =
		count > 0 ? static_cast<std::uint8_t>((sum + count / 2) / count) : 128;

	out.frame = frame;
	out.origin_x = roi.x; // requested origin preserved even when negative
	out.origin_y = roi.y;
	out.width = w;
	out.height = h;
	out.stride = w;
	// The loop below writes every element, so there is no need to pre-fill.
	out.gray.resize(size_t(w) * size_t(h));

	for (int y = 0; y < h; ++y) {
		int const sy = roi.y + y;
		bool const inside_y = sy >= 0 && sy < view.height;
		int const physical_y = LogicalRowToPhysicalRow(sy, view.height, view.flipped);
		for (int x = 0; x < w; ++x) {
			int const sx = roi.x + x;
			bool const inside =
				inside_y && sx >= 0 && sx < view.width;
			std::uint8_t value = fill;
			if (inside)
				value = BgraPixelToGray(view.data + int64_t(physical_y) * view.pitch + size_t(sx) * 4);
			out.gray[size_t(y) * size_t(w) + size_t(x)] = value;
		}
	}
	return FrameReadResult{FrameReadStatus::Ok, {}};
}

} // namespace aegisub::motion_track
