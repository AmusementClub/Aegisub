#include <main.h>

#include "../../src/perspective_quad_geometry.h"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace {
using namespace perspective;

Quad UnitQuad() {
	return {{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}};
}

void ExpectVecNear(Vec2 expected, Vec2 actual, double tolerance = 1.0e-10) {
	EXPECT_NEAR(expected.x, actual.x, tolerance);
	EXPECT_NEAR(expected.y, actual.y, tolerance);
}
}

TEST(perspective_quad_geometry, accepts_clockwise_convex_quad_in_y_down_coordinates) {
	Quad const quad {{{20.0, 10.0}, {180.0, 30.0}, {150.0, 130.0}, {5.0, 100.0}}};
	auto const validation = ValidateQuad(quad);
	EXPECT_TRUE(validation) << DescribeGeometryError(validation.error);
	EXPECT_GT(SignedArea(quad), 0.0);
	auto const center = QuadCenter(quad);
	ASSERT_TRUE(center);
	EXPECT_GT(center->x, 5.0);
	EXPECT_LT(center->x, 180.0);
}

TEST(perspective_quad_geometry, clip_segment_bounds_large_overlay_work_without_changing_direction) {
	Rect const viewport{.left = 0.0, .top = 0.0, .right = 100.0, .bottom = 80.0};
	auto const across = ClipSegmentRange({.x = -1.0e8, .y = 40.0}, {.x = 1.0e8, .y = 40.0}, viewport);
	ASSERT_TRUE(across);
	EXPECT_NEAR(0.5, (*across)[0], 1.0e-12);
	EXPECT_NEAR(0.5000005, (*across)[1], 1.0e-12);
	auto const backwards = ClipSegmentRange({.x = 1.0e8, .y = 40.0}, {.x = -1.0e8, .y = 40.0}, viewport);
	ASSERT_TRUE(backwards);
	EXPECT_NEAR(0.4999995, (*backwards)[0], 1.0e-12);
	EXPECT_NEAR(0.5, (*backwards)[1], 1.0e-12);
	EXPECT_FALSE(ClipSegmentRange({.x = -10.0, .y = 81.0}, {.x = 110.0, .y = 81.0}, viewport));
	auto const inside = ClipSegmentRange({.x = 10.0, .y = 20.0}, {.x = 20.0, .y = 30.0}, viewport);
	ASSERT_TRUE(inside);
	EXPECT_EQ((std::array<double, 2>{0.0, 1.0}), *inside);
	auto const corner = ClipSegmentRange({.x = -10.0, .y = -10.0}, {.x = 10.0, .y = 10.0}, viewport);
	ASSERT_TRUE(corner);
	EXPECT_EQ((std::array<double, 2>{0.5, 1.0}), *corner);
}

TEST(perspective_quad_geometry, rejects_reversed_winding_without_reordering_corners) {
	Quad const reversed {{{0.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}}};
	EXPECT_EQ(GeometryError::WrongWinding, ValidateQuad(reversed).error);
}

// The absolute-coordinate shoelace built ~1e16 products for a quad near the
// coordinate limit and cancelled the real area away inside them, so a unit
// square parked at 1e8 validated as AreaTooSmall. The origin-relative sum
// keeps the arithmetic at the quad's own scale.
TEST(perspective_quad_geometry, area_survives_coordinates_near_the_range_limit) {
	double const origin = 1.0e8;
	Quad const far_square {{
		{origin, origin},
		{origin + 1.0, origin},
		{origin + 1.0, origin + 1.0},
		{origin, origin + 1.0},
	}};
	EXPECT_NEAR(1.0, SignedArea(far_square), 1.0e-6);
	auto const validation = ValidateQuad(far_square);
	EXPECT_TRUE(validation) << DescribeGeometryError(validation.error);

	// Same quad, mirrored winding: the sign must survive the translation too.
	auto mirrored = far_square;
	std::swap(mirrored[0], mirrored[1]);
	std::swap(mirrored[2], mirrored[3]);
	EXPECT_NEAR(-1.0, SignedArea(mirrored), 1.0e-6);
	EXPECT_EQ(GeometryError::WrongWinding, ValidateQuad(mirrored).error);
}

TEST(perspective_quad_geometry, rejects_duplicate_tiny_and_non_finite_points) {
	auto duplicate = UnitQuad();
	duplicate[2] = duplicate[1];
	EXPECT_EQ(GeometryError::DuplicatePoint, ValidateQuad(duplicate).error);

	Quad const tiny {{{0.0, 0.0}, {1.0e-12, 0.0}, {1.0e-12, 1.0e-12}, {0.0, 1.0e-12}}};
	EXPECT_EQ(GeometryError::EdgeTooShort, ValidateQuad(tiny).error);

	auto non_finite = UnitQuad();
	non_finite[2].x = std::numeric_limits<double>::infinity();
	EXPECT_EQ(GeometryError::NonFinite, ValidateQuad(non_finite).error);
	non_finite[2].x = std::numeric_limits<double>::quiet_NaN();
	EXPECT_EQ(GeometryError::NonFinite, ValidateQuad(non_finite).error);

	auto out_of_range = UnitQuad();
	out_of_range[2].x = MaxAbsCoordinate * 2.0;
	EXPECT_EQ(GeometryError::CoordinateOutOfRange, ValidateQuad(out_of_range).error);
}

TEST(perspective_quad_geometry, rejects_concave_and_self_intersecting_quads) {
	Quad const concave {{{0.0, 0.0}, {2.0, 0.0}, {0.5, 0.5}, {0.0, 2.0}}};
	EXPECT_EQ(GeometryError::NonConvex, ValidateQuad(concave).error);

	Quad const bow_tie {{{0.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {2.0, 0.0}}};
	EXPECT_EQ(GeometryError::SelfIntersecting, ValidateQuad(bow_tie).error);

	Quad const collinear {{{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}}};
	EXPECT_EQ(GeometryError::SelfIntersecting, ValidateQuad(collinear).error);
}

TEST(perspective_quad_geometry, reports_area_too_small_before_homography_construction) {
	double const origin = 0.0;
	double const side = 1.0e-5;
	Quad const small_area {{
		{origin, origin},
		{origin + side, origin},
		{origin + side, origin + side},
		{origin, origin + side},
	}};

	auto const validation = ValidateQuad(small_area);
	EXPECT_EQ(GeometryError::AreaTooSmall, validation.error);
	EXPECT_GT(side, validation.linear_epsilon);
	EXPECT_LE(std::abs(SignedArea(small_area)), validation.area_epsilon);
	EXPECT_STREQ("quad area is too small", DescribeGeometryError(validation.error));

	auto const homography = MakeHomography({0.0, 0.0, 10.0, 10.0}, small_area);
	EXPECT_EQ(GeometryError::AreaTooSmall, homography.error);
	EXPECT_FALSE(homography);
}

TEST(perspective_quad_geometry, bounded_malformed_mutations_keep_stable_diagnostics) {
	struct MalformedCase {
		std::string_view name;
		Quad quad;
		GeometryError expected;
	};

	auto duplicate = UnitQuad();
	duplicate[2] = duplicate[1];
	Quad const reversed {{{0.0, 0.0}, {0.0, 4.0}, {6.0, 4.0}, {6.0, 0.0}}};
	Quad const short_edge {{{0.0, 0.0}, {1.0e-12, 0.0}, {4.0, 4.0}, {0.0, 4.0}}};
	Quad const concave {{{0.0, 0.0}, {6.0, 0.0}, {2.0, 1.0}, {0.0, 4.0}}};
	Quad const bow_tie {{{0.0, 0.0}, {6.0, 4.0}, {0.0, 4.0}, {6.0, 0.0}}};
	auto infinite = UnitQuad();
	infinite[2].y = std::numeric_limits<double>::infinity();
	auto not_a_number = UnitQuad();
	not_a_number[1].x = std::numeric_limits<double>::quiet_NaN();
	auto out_of_range = UnitQuad();
	out_of_range[3].x = -MaxAbsCoordinate - 1.0;

	std::array const cases {
		MalformedCase {"duplicate point", duplicate, GeometryError::DuplicatePoint},
		MalformedCase {"reversed winding", reversed, GeometryError::WrongWinding},
		MalformedCase {"short edge", short_edge, GeometryError::EdgeTooShort},
		MalformedCase {"concave", concave, GeometryError::NonConvex},
		MalformedCase {"self intersection", bow_tie, GeometryError::SelfIntersecting},
		MalformedCase {"infinite coordinate", infinite, GeometryError::NonFinite},
		MalformedCase {"NaN coordinate", not_a_number, GeometryError::NonFinite},
		MalformedCase {"out of range", out_of_range, GeometryError::CoordinateOutOfRange},
	};

	Rect const source {0.0, 0.0, 10.0, 10.0};
	for (auto const& test_case : cases) {
		auto const validation = ValidateQuad(test_case.quad);
		EXPECT_EQ(test_case.expected, validation.error) << test_case.name;
		EXPECT_STRNE("unknown geometry error", DescribeGeometryError(validation.error))
			<< test_case.name;
		EXPECT_FALSE(QuadCenter(test_case.quad)) << test_case.name;

		auto const homography = MakeHomography(source, test_case.quad);
		EXPECT_EQ(test_case.expected, homography.error) << test_case.name;
		EXPECT_FALSE(homography) << test_case.name;
	}
}

TEST(perspective_quad_geometry, matrix_inverse_round_trips_identity) {
	Matrix3 const matrix({2.0, 1.0, 4.0, -1.0, 3.0, 2.0, 0.01, -0.02, 1.0});
	auto const inverse = matrix.Inverse();
	ASSERT_TRUE(inverse);
	auto const product = matrix * *inverse;
	for (std::size_t row = 0; row < 3; ++row) {
		for (std::size_t column = 0; column < 3; ++column)
			EXPECT_NEAR(row == column ? 1.0 : 0.0, product(row, column), 1.0e-12);
	}
}

TEST(perspective_quad_geometry, homography_maps_all_source_corners_and_inverts) {
	Rect const source {10.0, 20.0, 110.0, 70.0};
	Quad const target {{{5.0, 7.0}, {170.0, 20.0}, {140.0, 100.0}, {-10.0, 80.0}}};
	auto const result = MakeHomography(source, target);
	ASSERT_TRUE(result) << DescribeGeometryError(result.error);
	auto const inverse = result.value.Inverse();
	ASSERT_TRUE(inverse);

	auto const source_corners = MakeQuad(source);
	for (std::size_t index = 0; index < target.size(); ++index) {
		auto const mapped = result.value.Map(source_corners[index]);
		ASSERT_TRUE(mapped);
		ExpectVecNear(target[index], *mapped, 1.0e-9);
		auto const round_trip = inverse->Map(*mapped);
		ASSERT_TRUE(round_trip);
		ExpectVecNear(source_corners[index], *round_trip, 1.0e-9);
	}
}

TEST(perspective_quad_geometry, projection_domain_rejects_denominator_crossing_and_zero) {
	Rect const domain {0.0, 0.0, 1.0, 1.0};
	Homography const crossing(Matrix3({
		1.0, 0.0, 0.0,
		0.0, 1.0, 0.0,
		1.0, 0.0, -0.5,
	}));
	EXPECT_EQ(GeometryError::ProjectionDomain, ValidateProjectionDomain(crossing, domain));

	Homography const zero_at_corner(Matrix3({
		1.0, 0.0, 0.0,
		0.0, 1.0, 0.0,
		1.0, 0.0, 0.0,
	}));
	EXPECT_EQ(GeometryError::ProjectionDomain, ValidateProjectionDomain(zero_at_corner, domain));
	EXPECT_FALSE(zero_at_corner.Map({0.0, 0.0}));

	Homography const negative_same_sign(Matrix3({
		-2.0e-20, 0.0, 0.0,
		0.0, -2.0e-20, 0.0,
		0.0, 0.0, -2.0e-20,
	}));
	EXPECT_EQ(GeometryError::None, ValidateProjectionDomain(negative_same_sign, domain));
	auto const mapped = negative_same_sign.Map({0.25, 0.75});
	ASSERT_TRUE(mapped);
	ExpectVecNear({0.25, 0.75}, *mapped);
}

TEST(perspective_quad_geometry, invalid_source_domain_and_target_leave_no_transform) {
	auto const invalid_source = MakeHomography({0.0, 0.0, 0.0, 10.0}, UnitQuad());
	EXPECT_EQ(GeometryError::InvalidDomain, invalid_source.error);

	auto target = UnitQuad();
	std::swap(target[1], target[3]);
	auto const invalid_target = MakeHomography({0.0, 0.0, 1.0, 1.0}, target);
	EXPECT_EQ(GeometryError::WrongWinding, invalid_target.error);
}
