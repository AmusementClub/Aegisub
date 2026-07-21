#pragma once

struct SkiaOverlayLogicalBounds {
	bool valid = false;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;

	bool operator==(SkiaOverlayLogicalBounds const&) const = default;
};

struct SkiaOverlayDeviceBounds {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	bool operator==(SkiaOverlayDeviceBounds const&) const = default;
	bool IsEmpty() const noexcept { return width <= 0 || height <= 0; }
};
