#pragma once

// Production implementations exist only inside a RunRawVideoBatch callback;
// core tests use SyntheticFrameReader. Fetching must be per-frame —
// prefetching a fixed ROI is forbidden because last_center moves after every
// accepted frame.

#include "types.h"

#include <string>

namespace aegisub::motion_track {

enum class FrameReadStatus : std::uint8_t {
	Ok,
	ProviderChanged,
	FrameUnavailable,
	DecodeError,
	Error,
};

struct FrameReadResult {
	FrameReadStatus status = FrameReadStatus::Error;
	std::string message;
};

class MotionFrameReader {
public:
	virtual ~MotionFrameReader() = default;

	// roi may extend past the frame bounds; out.origin_* keeps the requested
	// coordinates and out-of-bounds pixels are filled by the implementation.
	virtual FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) = 0;
};

} // namespace aegisub::motion_track
