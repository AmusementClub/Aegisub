#pragma once

#include <algorithm>

class AudioMarkerDragDeadZone final {
	int initial_x = 0;
	int dead_zone = 0;
	bool dragging = false;

public:
	AudioMarkerDragDeadZone() = default;
	AudioMarkerDragDeadZone(int initial_x, int dead_zone) { Reset(initial_x, dead_zone); }

	void Reset(int new_initial_x, int new_dead_zone) noexcept {
		initial_x = new_initial_x;
		dead_zone = std::max(0, new_dead_zone);
		dragging = false;
	}

	bool ShouldDrag(int current_x) noexcept {
		if (!dragging) {
			auto const delta = static_cast<long long>(current_x) - initial_x;
			dragging = delta < -dead_zone || delta > dead_zone;
		}
		return dragging;
	}
};
