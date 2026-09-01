#pragma once

// Instance lives only on the RunRawVideoBatch callback stack; it must never
// be owned by the session or outlive the callback. Gray conversion happens
// immediately per fetch — the provider-owned BGRA view dies at the next
// FetchBgra call.

#include "frame_reader.h"

#include "types.h"

#include "gray_convert.h"
#include "../async_video_provider.h"

#include <vector>

namespace aegisub::motion_track {

class RawBatchMotionFrameReader final : public MotionFrameReader {
public:
	explicit RawBatchMotionFrameReader(RawFrameAccess& access)
	: access(access) {}

	// Mirrors the production out-of-bounds rule: pixels outside the frame are
	// filled with the mean gray of the in-bounds intersection; a fully
	// off-frame ROI is filled with neutral 128 and still reports Ok — the
	// backend's flat/low-texture criteria turn that into Failed. Only invalid
	// frame indices produce FrameUnavailable (via FetchBgra bounds check).
	FrameReadResult FetchGray(int frame, RoiRect roi, GrayPatch& out) override;

private:
	RawFrameAccess& access;

};

} // namespace aegisub::motion_track
