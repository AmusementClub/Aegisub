#include <main.h>

#include "../../src/spline.h"

#include <limits>
#include <vector>

namespace {
void ExpectCurveEqual(SplineCurve const& expected, SplineCurve const& actual) {
	EXPECT_EQ(expected.type, actual.type);
	EXPECT_EQ(expected.p1, actual.p1);
	EXPECT_EQ(expected.p2, actual.p2);
	EXPECT_EQ(expected.p3, actual.p3);
	EXPECT_EQ(expected.p4, actual.p4);
}
}

TEST(spline, closest_parametric_point_uses_closing_segment_without_mutating_curves) {
	std::vector<SplineCurve> curves {
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(Vector2D(0, 0), Vector2D(10, 0)),
		SplineCurve(Vector2D(10, 0), Vector2D(10, 10)),
	};
	auto const original = curves;
	auto const *storage = curves.data();
	auto const capacity = curves.capacity();

	std::size_t curve_index = 0;
	float t = 0.f;
	Vector2D point;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 5), curve_index, t, point));

	EXPECT_EQ(curves.size(), curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(5, 5), point);
	EXPECT_EQ(storage, curves.data());
	EXPECT_EQ(capacity, curves.capacity());
	ASSERT_EQ(original.size(), curves.size());
	for (std::size_t index = 0; index < curves.size(); ++index)
		ExpectCurveEqual(original[index], curves[index]);
}

TEST(spline, closest_parametric_point_prefers_existing_curve_on_closing_segment_tie) {
	std::vector<SplineCurve> curves {
		SplineCurve(Vector2D(0, 0), Vector2D(10, 0)),
	};

	std::size_t curve_index = 0;
	float t = 0.f;
	Vector2D point;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 0), curve_index, t, point));

	EXPECT_EQ(0u, curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(5, 0), point);
}

TEST(spline, closest_parametric_point_uses_bicubic_endpoint_with_nonfinite_controls) {
	float const nan = std::numeric_limits<float>::quiet_NaN();
	std::vector<SplineCurve> curves {
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(
			Vector2D(0, 0),
			Vector2D(nan, nan),
			Vector2D(nan, nan),
			Vector2D(10, 10)),
	};

	std::size_t curve_index = 0;
	float t = 0.f;
	Vector2D point;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 5), curve_index, t, point));

	EXPECT_EQ(curves.size(), curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(5, 5), point);
}

TEST(spline, closest_parametric_point_rejects_empty_curve_set) {
	std::vector<SplineCurve> curves;
	std::size_t curve_index = 42;
	float t = 0.25f;
	Vector2D point(3, 4);

	EXPECT_FALSE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 5), curve_index, t, point));
	EXPECT_EQ(0u, curve_index);
	EXPECT_FLOAT_EQ(0.25f, t);
	EXPECT_EQ(Vector2D(3, 4), point);
}
