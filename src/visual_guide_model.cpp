#include "visual_guide_model.h"

#include <cmath>
#include <numbers>

namespace {
double NormalizeZero(double value) noexcept {
	return value == 0.0 ? 0.0 : value;
}
}

bool IsFiniteVisualGuidePoint(VisualGuidePoint point) noexcept {
	return std::isfinite(point.x) && std::isfinite(point.y);
}

bool IsValidVisualGuideData(VisualGuide const& guide) noexcept {
	return IsFiniteVisualGuidePoint(guide.first)
		&& IsFiniteVisualGuidePoint(guide.second);
}

bool IsValidVisualGuide(VisualGuide const& guide) noexcept {
	return !guide.id.empty() && IsValidVisualGuideData(guide);
}

VisualGuideMetrics CalculateVisualGuideMetrics(VisualGuide const& guide) noexcept {
	if (!IsValidVisualGuideData(guide))
		return {};

	auto const delta_x = NormalizeZero(guide.second.x - guide.first.x);
	auto const delta_y = NormalizeZero(guide.second.y - guide.first.y);
	if (delta_x == 0.0 && delta_y == 0.0)
		return {};

	return {
		delta_x,
		delta_y,
		std::hypot(delta_x, delta_y),
		std::atan2(delta_y, delta_x) * 180.0 / std::numbers::pi,
	};
}
