#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace aegisub::audio {

inline int CenteredScrollLeft(int time_ms, int viewport_width, double milliseconds_per_pixel) noexcept
{
	if (viewport_width <= 0
		|| !std::isfinite(milliseconds_per_pixel)
		|| milliseconds_per_pixel <= 0.0)
		return 0;

	auto const target = std::max(time_ms, 0) / milliseconds_per_pixel - viewport_width / 2.0;
	return static_cast<int>(std::clamp(
		target,
		0.0,
		static_cast<double>(std::numeric_limits<int>::max())));
}

}
