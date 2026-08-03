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
	/// Horizontal shear coefficient (Δx/Δy); 0.0 when Δy is 0. Matches the
	/// \fax matrix entry. The display layer renders the axis-parallel case as
	/// a dash rather than this placeholder.
	double shear_x = 0.0;
	/// Vertical shear coefficient (Δy/Δx); 0.0 when Δx is 0. Matches \fay.
	double shear_y = 0.0;
	/// Angle to the horizontal axis, atan2(Δy,Δx) in degrees. Same value as
	/// angle_degrees; kept as a distinct field for label clarity.
	double angle_horizontal = 0.0;
	/// Angle to the vertical axis, atan2(Δx,Δy) in degrees.
	double angle_vertical = 0.0;

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
