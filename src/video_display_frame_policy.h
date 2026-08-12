#pragma once

inline bool ShouldIgnoreVideoDisplayFrameReady(
	bool free_size,
	bool is_current_display) noexcept {
	return !free_size && !is_current_display;
}
