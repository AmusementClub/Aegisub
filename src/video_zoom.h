// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <algorithm>
#include <cmath>

constexpr double kVideoZoomStep = 0.125;

inline double ClampVideoZoomToPresetRange(double zoom, int preset_count) {
	if (preset_count <= 0)
		return zoom;

	return std::clamp(zoom, kVideoZoomStep, kVideoZoomStep * preset_count);
}

inline double SnapVideoZoomToNearestPreset(double zoom, int preset_count) {
	if (preset_count <= 0)
		return zoom;

	int preset_index = std::clamp(static_cast<int>(std::lround(zoom / kVideoZoomStep)), 1, preset_count);
	return preset_index * kVideoZoomStep;
}

inline double AdvanceDetachedVideoZoomByWheel(double current_zoom, int wheel_steps, int preset_count) {
	if (wheel_steps == 0 || preset_count <= 0)
		return current_zoom;

	double zoom = ClampVideoZoomToPresetRange(current_zoom, preset_count);
	double snapped_zoom = SnapVideoZoomToNearestPreset(zoom, preset_count);

	// If detached resize left us on an in-between value, consume the first
	// wheel notch by snapping back to the nearest preset before continuing.
	if (std::abs(zoom - snapped_zoom) > 1e-9) {
		zoom = snapped_zoom;
		wheel_steps += wheel_steps > 0 ? -1 : 1;
		if (wheel_steps == 0)
			return zoom;
	}

	return ClampVideoZoomToPresetRange(zoom + wheel_steps * kVideoZoomStep, preset_count);
}
