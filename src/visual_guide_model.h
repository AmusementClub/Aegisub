#pragma once

#include <string>

struct VisualGuidePoint {
	double x = 0.0;
	double y = 0.0;

	bool operator==(VisualGuidePoint const&) const = default;
};

struct VisualGuideMetrics {
	double delta_x = 0.0;
	double delta_y = 0.0;
	double distance = 0.0;
	double angle_degrees = 0.0;

	bool operator==(VisualGuideMetrics const&) const = default;
};

/// Session-only measurement segment stored in script (PlayRes) pixels.
struct VisualGuide {
	std::string id;
	VisualGuidePoint first;
	VisualGuidePoint second;

	bool operator==(VisualGuide const&) const = default;
};

[[nodiscard]] bool IsFiniteVisualGuidePoint(VisualGuidePoint point) noexcept;

/// Check the guide data supplied to the controller before it assigns its stable
/// session ID.
[[nodiscard]] bool IsValidVisualGuideData(VisualGuide const& guide) noexcept;

/// Check a guide which is already stored in a controller snapshot.
[[nodiscard]] bool IsValidVisualGuide(VisualGuide const& guide) noexcept;

[[nodiscard]] VisualGuideMetrics CalculateVisualGuideMetrics(
	VisualGuide const& guide) noexcept;
