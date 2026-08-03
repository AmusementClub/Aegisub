#include <main.h>

#include "../../src/visual_guide_model.h"

#include <limits>

namespace {
VisualGuide Measurement(double first_x, double first_y, double second_x, double second_y) {
	VisualGuide guide;
	guide.first = { first_x, first_y };
	guide.second = { second_x, second_y };
	return guide;
}
}

TEST(visual_guide_model, measurement_metrics_follow_screen_coordinate_angle_definition) {
	auto const metrics = CalculateVisualGuideMetrics(Measurement(10.0, 20.0, 13.0, 24.0));

	EXPECT_DOUBLE_EQ(3.0, metrics.delta_x);
	EXPECT_DOUBLE_EQ(4.0, metrics.delta_y);
	EXPECT_DOUBLE_EQ(5.0, metrics.distance);
	EXPECT_NEAR(53.13010235415598, metrics.angle_degrees, 1e-12);
	// shear_x = dx/dy, shear_y = dy/dx (mirrors the \fax / \fay shear tags).
	EXPECT_NEAR(0.75, metrics.shear_x, 1e-12);
	EXPECT_NEAR(4.0 / 3.0, metrics.shear_y, 1e-12);
	// angle_horizontal mirrors angle_degrees; angle_vertical is the Y-axis angle.
	EXPECT_NEAR(53.13010235415598, metrics.angle_horizontal, 1e-12);
	EXPECT_NEAR(36.86989764584402, metrics.angle_vertical, 1e-12);
}

TEST(visual_guide_model, shear_is_zero_on_axis_aligned_segments) {
	// Horizontal: dy == 0, so shear_x denominator vanishes -> 0; shear_y = 0/dx = 0.
	auto const horizontal = CalculateVisualGuideMetrics(Measurement(1.0, 5.0, 4.0, 5.0));
	EXPECT_DOUBLE_EQ(0.0, horizontal.shear_x);
	EXPECT_DOUBLE_EQ(0.0, horizontal.shear_y);

	// Vertical: dx == 0, so shear_y denominator vanishes -> 0; shear_x = 0/dy = 0.
	auto const vertical = CalculateVisualGuideMetrics(Measurement(5.0, 1.0, 5.0, 4.0));
	EXPECT_DOUBLE_EQ(0.0, vertical.shear_x);
	EXPECT_DOUBLE_EQ(0.0, vertical.shear_y);
}

TEST(visual_guide_model, measurement_metrics_cover_cardinal_and_zero_length_segments) {
	struct Case {
		double second_x;
		double second_y;
		double expected_angle;
	};
	Case const cases[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 90.0 },
		{ -1.0, 0.0, 180.0 },
		{ 0.0, -1.0, -90.0 },
		{ -1.0, -1.0, -135.0 },
	};

	for (auto const& test : cases) {
		auto const metrics = CalculateVisualGuideMetrics(
			Measurement(0.0, 0.0, test.second_x, test.second_y));
		EXPECT_NEAR(test.expected_angle, metrics.angle_degrees, 1e-12);
	}

	auto const zero = CalculateVisualGuideMetrics(Measurement(7.0, -3.0, 7.0, -3.0));
	EXPECT_EQ(VisualGuideMetrics{}, zero);
}

TEST(visual_guide_model, validation_rejects_nonfinite_and_empty_id) {
	VisualGuide guide = Measurement(0.0, 0.0, 1.0, 1.0);
	EXPECT_TRUE(IsValidVisualGuideData(guide));
	EXPECT_FALSE(IsValidVisualGuide(guide));

	guide.id = "guide-1";
	EXPECT_TRUE(IsValidVisualGuide(guide));

	guide.first.x = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(IsValidVisualGuideData(guide));
	EXPECT_EQ(VisualGuideMetrics{}, CalculateVisualGuideMetrics(guide));

	guide = Measurement(0.0, 0.0, std::numeric_limits<double>::infinity(), 1.0);
	EXPECT_FALSE(IsValidVisualGuideData(guide));
}
