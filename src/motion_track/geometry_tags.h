#pragma once

#include "types.h"

#include <string>
#include <string_view>

namespace aegisub::motion_track {

struct GeometryTagResult {
	bool success = true;
	bool has_geometry = false;
	bool has_animated_clip = false;
	std::string text;
	std::string error;
};

// Transform absolute \org and \clip/\iclip coordinates with a matrix already
// expressed in script (PlayRes) coordinates. Unrelated bytes are preserved.
// Affine curves stay cubic; projective curves are subdivided with a convex-hull
// error bound in script pixels, including output drawing quantization.
// Rectangular clips inside \t remain supported only when the transformed clip
// is also a representable rectangle: libass cannot animate vector clips.
// Converting a rectangle in an event with additional clip tags is rejected,
// because vector and rectangular clips have different renderer precedence.
// On failure text is the original event, so callers can reject atomically.
GeometryTagResult TransformGeometryTags(std::string_view text,
										TrackTransform const& script_transform, double curve_tolerance = 0.1);

} // namespace aegisub::motion_track
