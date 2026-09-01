#include <main.h>

#include "../../src/perspective_forward.h"

#include <cmath>
#include <utility>

namespace {
using namespace perspective;

ForwardInput IdentityInput() {
	ForwardInput input;
	input.play_resolution = {1920.0, 1080.0};
	input.bounds = {{0.0, 0.0, 120.0, 60.0}, BoundsKind::Text};
	input.state.alignment = 7;
	input.state.position = {100.0, 200.0};
	input.state.origin = input.state.position;
	return input;
}

void ExpectVecNear(Vec2 expected, Vec2 actual, double tolerance = 1.0e-9) {
	EXPECT_NEAR(expected.x, actual.x, tolerance);
	EXPECT_NEAR(expected.y, actual.y, tolerance);
}
}

TEST(perspective_forward, identity_forward_honors_all_nine_alignments) {
	for (int alignment = 1; alignment <= 9; ++alignment) {
		auto input = IdentityInput();
		input.state.alignment = alignment;
		auto const result = ForwardQuad(input);
		ASSERT_TRUE(result) << DescribeForwardError(result.error);

		double const horizontal = alignment % 3 == 2 ? -60.0 : alignment % 3 == 0 ? -120.0 : 0.0;
		double const vertical = alignment <= 3 ? -60.0 : alignment <= 6 ? -30.0 : 0.0;
		ExpectVecNear({100.0 + horizontal, 200.0 + vertical}, result.quad[0]);
		ExpectVecNear({220.0 + horizontal, 200.0 + vertical}, result.quad[1]);
		ExpectVecNear({220.0 + horizontal, 260.0 + vertical}, result.quad[2]);
		ExpectVecNear({100.0 + horizontal, 260.0 + vertical}, result.quad[3]);
	}
}

TEST(perspective_forward, drawing_bounds_are_transformed_from_their_real_origin) {
	auto input = IdentityInput();
	input.bounds = {{10.0, 20.0, 110.0, 70.0}, BoundsKind::Drawing};
	input.state.position = {0.0, 0.0};
	input.state.origin = input.state.position;
	input.state.scale_x = 200.0;
	input.state.scale_y = 50.0;
	input.state.shear_x = 0.5;
	auto const result = ForwardQuad(input);
	ASSERT_TRUE(result) << DescribeForwardError(result.error);
	ExpectVecNear({40.0, 10.0}, result.quad[0]);
	ExpectVecNear({240.0, 10.0}, result.quad[1]);
	ExpectVecNear({290.0, 35.0}, result.quad[2]);
	ExpectVecNear({90.0, 35.0}, result.quad[3]);
}

TEST(perspective_forward, drawing_alignment_uses_renderer_metrics_not_tight_extrema) {
	auto input = IdentityInput();
	input.bounds = {
		{0.0, 0.0, 10.0, 7.5}, BoundsKind::Drawing, Resolution {10.0, 10.0}};
	input.state.alignment = 2;
	input.state.position = {0.0, 0.0};
	input.state.origin = input.state.position;
	auto const result = ForwardQuad(input);
	ASSERT_TRUE(result) << DescribeForwardError(result.error);
	ExpectVecNear({-5.0, -10.0}, result.quad[0]);
	ExpectVecNear({5.0, -2.5}, result.quad[2]);
}

TEST(perspective_forward, rotations_follow_renderer_zxy_sign_convention) {
	auto input = IdentityInput();
	input.bounds = {{0.0, 0.0, 100.0, 100.0}, BoundsKind::Text};
	input.state.position = {0.0, 0.0};
	input.state.origin = input.state.position;
	input.state.rotation_z = 90.0;
	auto const z_result = ForwardQuad(input);
	ASSERT_TRUE(z_result);
	ExpectVecNear({0.0, 0.0}, z_result.quad[0]);
	ExpectVecNear({0.0, -100.0}, z_result.quad[1]);

	input.state.rotation_z = 0.0;
	input.state.rotation_y = 45.0;
	auto const y_result = ForwardQuad(input);
	ASSERT_TRUE(y_result);
	double const expected_x = 100.0 * std::cos(3.14159265358979323846 / 4.0)
		* y_result.camera_distance
		/ (y_result.camera_distance + 100.0 * std::sin(3.14159265358979323846 / 4.0));
	EXPECT_NEAR(expected_x, y_result.quad[1].x, 1.0e-9);
	EXPECT_NEAR(0.0, y_result.quad[1].y, 1.0e-9);
}

TEST(perspective_forward, combined_transform_matches_hardcoded_renderer_golden) {
	auto input = IdentityInput();
	input.layout_resolution = Resolution {1280.0, 720.0};
	input.bounds = {{20.0, 30.0, 180.0, 120.0}, BoundsKind::Text};
	input.state.alignment = 5;
	input.state.position = {420.0, 310.0};
	input.state.origin = Vec2 {365.0, 280.0};
	input.state.scale_x = 137.5;
	input.state.scale_y = 72.0;
	input.state.shear_x = 0.18;
	input.state.shear_y = -0.11;
	input.state.rotation_x = 23.0;
	input.state.rotation_y = -31.0;
	input.state.rotation_z = 17.0;

	auto const result = ForwardQuad(input);
	ASSERT_TRUE(result) << DescribeForwardError(result.error);
	EXPECT_DOUBLE_EQ(1280.0, result.resolved_layout.width);
	EXPECT_DOUBLE_EQ(720.0, result.resolved_layout.height);
	EXPECT_DOUBLE_EQ(468.75, result.camera_distance);

	// Calculated independently from the documented ASS transform order.
	constexpr double tolerance = 1.0e-9;
	ExpectVecNear({348.374474987811, 300.926497163162}, result.quad[0], tolerance);
	ExpectVecNear({577.804596591947, 220.181605102538}, result.quad[1], tolerance);
	ExpectVecNear({633.238950755519, 282.145527338954}, result.quad[2], tolerance);
	ExpectVecNear({372.348373723662, 358.611032165689}, result.quad[3], tolerance);

	auto const center = result.transform.Map({100.0, 75.0});
	ASSERT_TRUE(center);
	ExpectVecNear({469.850113876879, 292.917670150936}, *center, tolerance);
}

TEST(perspective_forward, layout_resolution_and_video_fallback_change_camera_distance) {
	auto input = IdentityInput();
	input.state.rotation_y = 45.0;
	input.layout_resolution = Resolution {1920.0, 540.0};
	auto explicit_layout = ForwardQuad(input);
	ASSERT_TRUE(explicit_layout);
	EXPECT_DOUBLE_EQ(625.0, explicit_layout.camera_distance);

	input.layout_resolution = Resolution {0.0, 0.0};
	input.video_storage_resolution = Resolution {1920.0, 540.0};
	auto video_fallback = ForwardQuad(input);
	ASSERT_TRUE(video_fallback);
	EXPECT_DOUBLE_EQ(625.0, video_fallback.camera_distance);

	input.video_storage_resolution.reset();
	auto play_fallback = ForwardQuad(input);
	ASSERT_TRUE(play_fallback);
	EXPECT_DOUBLE_EQ(312.5, play_fallback.camera_distance);
}

TEST(perspective_forward, unsupported_bounds_and_invalid_state_have_stable_diagnostics) {
	for (auto kind_and_error : {
		std::pair {BoundsKind::MixedRuns, ForwardError::UnsupportedMixedBounds},
		std::pair {BoundsKind::Dynamic, ForwardError::UnsupportedDynamicBounds},
		std::pair {BoundsKind::FontUnavailable, ForwardError::UnsupportedFontBounds},
	}) {
		auto input = IdentityInput();
		input.bounds.kind = kind_and_error.first;
		auto const result = ForwardQuad(input);
		EXPECT_EQ(kind_and_error.second, result.error);
		EXPECT_STRNE("unknown forward error", DescribeForwardError(result.error));
	}

	auto invalid_alignment = IdentityInput();
	invalid_alignment.state.alignment = 0;
	EXPECT_EQ(ForwardError::InvalidAlignment, ForwardQuad(invalid_alignment).error);

	auto mirrored = IdentityInput();
	mirrored.state.scale_x = -100.0;
	EXPECT_EQ(ForwardError::MirroredTransform, ForwardQuad(mirrored).error);

	auto singular_shear = IdentityInput();
	singular_shear.state.shear_x = 2.0;
	singular_shear.state.shear_y = 0.5;
	EXPECT_EQ(ForwardError::SingularTransform, ForwardQuad(singular_shear).error);

	auto bad_time = IdentityInput();
	bad_time.state.event_time_ms = -1;
	EXPECT_EQ(ForwardError::InvalidEventTime, ForwardQuad(bad_time).error);

	auto bad_alignment_extent = IdentityInput();
	bad_alignment_extent.bounds.alignment_extent = Resolution {0.0, 10.0};
	EXPECT_EQ(ForwardError::InvalidBounds, ForwardQuad(bad_alignment_extent).error);
}

TEST(perspective_forward, projection_domain_crossing_is_rejected_before_quad_output) {
	auto input = IdentityInput();
	input.bounds = {{-400.0, 0.0, 400.0, 100.0}, BoundsKind::Text};
	input.state.position = {0.0, 0.0};
	input.state.origin = input.state.position;
	input.state.rotation_y = 90.0;
	auto const result = ForwardQuad(input);
	EXPECT_EQ(ForwardError::ProjectionDomain, result.error);
}

TEST(perspective_forward, behind_camera_corners_are_rejected) {
	// All four corners behind the camera pass no projection-domain rule by
	// accident: libass's normalized frame (restore_transform) pins the
	// nearest bbox corner at unit depth, so it never renders in-bbox text
	// from behind the camera, and there is no clamp target to follow.
	// Projecting anyway would mirror the quad through the camera.
	auto input = IdentityInput();
	input.state.origin = {700.0, 200.0}; // rotation axis far right of the quad
	input.state.rotation_y = 80.0;
	auto const result = ForwardQuad(input);
	EXPECT_EQ(ForwardError::ProjectionDomain, result.error);
}

TEST(perspective_forward, small_positive_denominators_render_true_perspective) {
	// Every denominator positive but far below the camera distance: a
	// legitimate perspective gradient libass also renders as-is (its
	// normalized frame keeps the nearest corner at unit depth, far above
	// any floor). The projection must keep the per-corner factors -- the
	// near edge renders visibly larger than the far edge -- rather than
	// flattening the quad into one uniform scale.
	auto input = IdentityInput();
	input.state.origin = {1840.0, 200.0}; // rotation axis far right of the quad
	input.state.rotation_y = 10.0;
	auto const result = ForwardQuad(input);
	ASSERT_TRUE(result) << DescribeForwardError(result.error);

	// Left corners sit nearer the camera (denominator ~10) than the right
	// ones (~31), so the left edge projects roughly three times the right
	// edge. A near-plane clamp would flatten both to the same length.
	double const left_edge = std::fabs(result.quad[3].y - result.quad[0].y);
	double const right_edge = std::fabs(result.quad[2].y - result.quad[1].y);
	EXPECT_GT(left_edge, 2.0 * right_edge);
}

TEST(perspective_forward, returned_homography_maps_the_source_center) {
	auto input = IdentityInput();
	input.state.scale_x = 125.0;
	input.state.scale_y = 80.0;
	input.state.shear_x = 0.2;
	input.state.rotation_z = 25.0;
	auto const result = ForwardQuad(input);
	ASSERT_TRUE(result) << DescribeForwardError(result.error);
	auto const mapped = result.transform.Map({60.0, 30.0});
	ASSERT_TRUE(mapped);
	EXPECT_NEAR((result.quad[0].x + result.quad[1].x + result.quad[2].x + result.quad[3].x) / 4.0,
		mapped->x, 30.0);
	EXPECT_NEAR((result.quad[0].y + result.quad[1].y + result.quad[2].y + result.quad[3].y) / 4.0,
		mapped->y, 30.0);
}
