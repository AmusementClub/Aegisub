#include <main.h>

#include "../../src/perspective_tag_solver.h"

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace perspective;

ForwardInput BaseInput() {
	ForwardInput input;
	input.play_resolution = {1920.0, 1080.0};
	input.layout_resolution = Resolution {1920.0, 1080.0};
	input.bounds = {{0.0, 0.0, 200.0, 80.0}, BoundsKind::Text};
	input.state.alignment = 7;
	input.state.position = {400.0, 300.0};
	return input;
}

Quad TargetFrom(ForwardInput input, EvaluatedTransformState const& state) {
	input.state = state;
	auto const forward = ForwardQuad(input);
	EXPECT_TRUE(forward) << DescribeForwardError(forward.error);
	return forward.quad;
}

SolverResult SolveTo(ForwardInput const& source, EvaluatedTransformState const& target_state) {
	SolverInput input;
	input.source = source;
	input.target = TargetFrom(source, target_state);
	return SolvePerspectiveTags(input);
}

int FractionalDigits(std::string_view value) {
	auto const dot = value.find('.');
	if (dot == std::string_view::npos)
		return 0;
	return static_cast<int>(value.size() - dot - 1);
}

void ExpectPointAtMostDecimals(std::string const& point, int max) {
	ASSERT_FALSE(point.empty());
	ASSERT_EQ('(', point.front()) << point;
	ASSERT_EQ(')', point.back()) << point;
	auto const inner = point.substr(1, point.size() - 2);
	auto const comma = inner.find(',');
	ASSERT_NE(std::string::npos, comma) << point;
	EXPECT_LE(FractionalDigits(inner.substr(0, comma)), max) << point;
	EXPECT_LE(FractionalDigits(inner.substr(comma + 1)), max) << point;
}

void ExpectSerializedAtMost(SerializedTransformState const& serialized, int max) {
	ExpectPointAtMostDecimals(serialized.position, max);
	if (serialized.origin)
		ExpectPointAtMostDecimals(*serialized.origin, max);
	EXPECT_LE(FractionalDigits(serialized.scale_x), max) << serialized.scale_x;
	EXPECT_LE(FractionalDigits(serialized.scale_y), max) << serialized.scale_y;
	EXPECT_LE(FractionalDigits(serialized.shear_x), max) << serialized.shear_x;
	EXPECT_LE(FractionalDigits(serialized.shear_y), max) << serialized.shear_y;
	EXPECT_LE(FractionalDigits(serialized.rotation_x), max) << serialized.rotation_x;
	EXPECT_LE(FractionalDigits(serialized.rotation_y), max) << serialized.rotation_y;
	EXPECT_LE(FractionalDigits(serialized.rotation_z), max) << serialized.rotation_z;
}
}

TEST(perspective_tag_solver, ass_number_format_is_compact_and_never_scientific) {
	EXPECT_EQ("12.34", FormatAssNumber(12.340000, 6));
	EXPECT_EQ("0", FormatAssNumber(-0.00001, 3));
	EXPECT_EQ("1000000", FormatAssNumber(1.0e6, 6));
	EXPECT_EQ("(12.5,0)", FormatAssPoint({12.5, -0.00001}, 3));
	EXPECT_EQ(std::string::npos, FormatAssNumber(1234567.25, 2).find_first_of("eE"));
}

TEST(perspective_tag_solver, decimal_place_clamp_is_0_to_6) {
	EXPECT_EQ(0, ClampPerspectiveDecimalPlaces(-3));
	EXPECT_EQ(0, ClampPerspectiveDecimalPlaces(0));
	EXPECT_EQ(2, ClampPerspectiveDecimalPlaces(2));
	EXPECT_EQ(6, ClampPerspectiveDecimalPlaces(6));
	EXPECT_EQ(6, ClampPerspectiveDecimalPlaces(9));
}

TEST(perspective_tag_solver, no_op_preserves_existing_double_shear_and_origin) {
	auto source = BaseInput();
	source.state.origin = Vec2 {410.0, 290.0};
	source.state.scale_x = 115.0;
	source.state.scale_y = 90.0;
	source.state.shear_x = 0.2;
	source.state.shear_y = 0.1;
	source.state.rotation_x = 8.0;
	source.state.rotation_y = -6.0;
	source.state.rotation_z = 12.0;
	auto const result = SolveTo(source, source.state);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::NoOp, result.candidate->family);
	EXPECT_EQ(0, result.candidate->score.changed_tag_count);
	EXPECT_EQ(1, result.candidate->score.explicit_origin_penalty);
	// The default verdict on success: any other value would make the UI
	// diagnostic blame a stage that never ran.
	EXPECT_EQ(NoFeasibleReason::None, result.no_feasible_reason);
	EXPECT_DOUBLE_EQ(source.state.shear_x, result.candidate->state.shear_x);
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(source.state.origin->x, result.candidate->state.origin->x);
}

TEST(perspective_tag_solver, changed_geometry_prefers_no_origin_over_fewer_mutations) {
	auto source = BaseInput();
	source.state.origin = Vec2 {420.0, 310.0};
	source.state.scale_x = 120.0;
	source.state.scale_y = 85.0;
	source.state.shear_x = 0.18;
	source.state.shear_y = 0.07;
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	source.state.rotation_z = 9.0;
	auto target = source.state;
	target.position = target.position + Vec2 {35.0, -22.0};
	target.origin = *target.origin + Vec2 {35.0, -22.0};
	auto const result = SolveTo(source, target);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_TRUE(
		result.candidate->family == CandidateFamily::ProjectiveImplicitFax
		|| result.candidate->family == CandidateFamily::ProjectiveImplicitFay);
	EXPECT_FALSE(result.candidate->state.origin);
	EXPECT_EQ(0, result.candidate->score.explicit_origin_penalty);
	EXPECT_LE(result.candidate->max_error, 0.1);
}

TEST(perspective_tag_solver, fax_frz_only_keeps_the_declared_plane_and_reports_a_retilt_as_shortfall) {
	auto source = BaseInput();
	source.state.origin = Vec2 {420.0, 310.0};
	source.state.scale_x = 120.0;
	source.state.scale_y = 85.0;
	source.state.shear_x = 0.18;
	source.state.shear_y = 0.07;
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	source.state.rotation_z = 9.0;
	// The target must change the plane itself: a pure translation of this line
	// is reachable inside the declared plane by the fay-preserving refit (see
	// the move test below), so only a re-tilt keeps this a refusal.
	auto desired = source.state;
	desired.position = desired.position + Vec2 {35.0, -22.0};
	desired.rotation_x = 26.0;
	auto const target = TargetFrom(source, desired);

	SolverInput input {source, target};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	// A \org-bearing line has no policy-clean affine family: the filter only
	// passes states that keep the restricted tags exactly as written, so the
	// plane refit is the whole reachable set. It freezes org, fay, frx and fry
	// at the declared values and cannot re-tilt the plane -- the re-tilt comes
	// back as the reported shortfall instead of a refusal.
	EXPECT_EQ(CandidateFamily::CurrentPlaneRefit, result.candidate->family);
	EXPECT_TRUE(result.candidate->Snapped());
	EXPECT_GT(result.candidate->snap_error, input.max_error);
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(source.state.origin->x, result.candidate->state.origin->x);
	EXPECT_DOUBLE_EQ(source.state.origin->y, result.candidate->state.origin->y);
	EXPECT_DOUBLE_EQ(source.state.rotation_x, result.candidate->state.rotation_x);
	EXPECT_DOUBLE_EQ(source.state.rotation_y, result.candidate->state.rotation_y);
}

// The very drag that used to be refused: a translated line carrying \org and
// \fay under the restricted policy. The fay-preserving plane refit holds org,
// \fay, frx and fry exactly as written and tracks the translation through the
// free in-plane tags, so the switch now answers the drag instead of blaming
// itself. (Back when the plane refit hard-zeroed the fay this expected
// NoFeasibleCandidate + RepresentationPolicy: the only refit died at the
// policy filter for rewriting a restricted tag.)
TEST(perspective_tag_solver, fax_frz_only_moves_an_org_fay_line_inside_its_declared_plane) {
	auto source = BaseInput();
	source.state.origin = Vec2 {420.0, 310.0};
	source.state.scale_x = 120.0;
	source.state.scale_y = 85.0;
	source.state.shear_x = 0.18;
	source.state.shear_y = 0.07;
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	source.state.rotation_z = 9.0;
	auto target = TargetFrom(source, source.state);
	for (auto& point : target)
		point = point + Vec2 {35.0, -22.0};

	SolverInput input {source, target};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::CurrentPlaneRefit, result.candidate->family);
	// Every restricted tag numerically identical to the source: the solve
	// happened entirely inside the plane the line already declares.
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(source.state.origin->x, result.candidate->state.origin->x);
	EXPECT_DOUBLE_EQ(source.state.origin->y, result.candidate->state.origin->y);
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	EXPECT_DOUBLE_EQ(source.state.rotation_x, result.candidate->state.rotation_x);
	EXPECT_DOUBLE_EQ(source.state.rotation_y, result.candidate->state.rotation_y);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		source.state, result.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, similarity_candidate_uses_uniform_scale_and_z_rotation) {
	auto source = BaseInput();
	auto target = source.state;
	target.position = {520.0, 420.0};
	target.scale_x = 135.0;
	target.scale_y = 135.0;
	target.rotation_z = 27.0;
	auto const result = SolveTo(source, target);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::Similarity, result.candidate->family);
	EXPECT_NEAR(result.candidate->state.scale_x, result.candidate->state.scale_y, 1.0e-9);
	EXPECT_NEAR(0.0, result.candidate->state.shear_x, 1.0e-9);
	EXPECT_LE(result.candidate->max_error, 0.1);
}

TEST(perspective_tag_solver, preserve_scale_allows_translation_without_changing_scale) {
	auto source = BaseInput();
	auto target = source.state;
	target.position = {525.0, 415.0};
	SolverInput input {source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

// A uniform size change is the one thing a scale lock ignores: the drawn quad
// is rescaled to the area the frozen \fscx/\fscy can produce, and the solve
// succeeds without touching scale. An aspect change is shape, not size, and
// comes back as a reported shortfall.
TEST(perspective_tag_solver, preserve_scale_ignores_uniform_size_and_reports_aspect_shortfall) {
	auto source = BaseInput();

	auto uniform = source.state;
	uniform.scale_x = 160.0;
	uniform.scale_y = 160.0;
	SolverInput uniform_input{source, TargetFrom(source, uniform)};
	uniform_input.scale_policy = PerspectiveScalePolicy::Preserve;
	auto const solved = SolvePerspectiveTags(uniform_input);
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, solved.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, solved.candidate->state.scale_y);
	EXPECT_FALSE(solved.candidate->Snapped());

	auto aspect = source.state;
	aspect.scale_x = 175.0;
	aspect.scale_y = 160.0;
	SolverInput aspect_input{source, TargetFrom(source, aspect)};
	aspect_input.scale_policy = PerspectiveScalePolicy::Preserve;
	auto const result = SolvePerspectiveTags(aspect_input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	// An aspect change is genuinely about shape: the nearest normalized quad
	// still misses by the aspect difference, and that shortfall is reported
	// rather than silently rounded away.
	EXPECT_TRUE(result.candidate->Snapped());
	EXPECT_GT(result.candidate->snap_error, aspect_input.max_error);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
}

TEST(perspective_tag_solver, fit_scale_still_solves_target_that_requires_scaling) {
	auto source = BaseInput();
	auto target = source.state;
	target.scale_x = 175.0;
	target.scale_y = 160.0;
	SolverInput input {source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Fit;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_NE(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_NE(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, preserve_scale_keeps_rectangular_targets_rectangular) {
	for (double const rotation : {0.0, 27.0, -90.0}) {
		for (auto const scales : {Vec2{.x = 44.0, .y = 192.5}, Vec2{.x = 240.0, .y = 65.0}}) {
			SCOPED_TRACE(rotation);
			SCOPED_TRACE(scales.x);
			auto source = BaseInput();
			source.state.alignment = 5;
			auto desired = source.state;
			desired.position = {.x = 500.0, .y = 300.0};
			desired.rotation_z = rotation;
			desired.scale_x = scales.x;
			desired.scale_y = scales.y;
			SolverInput input{.source = source, .target = TargetFrom(source, desired)};
			input.scale_policy = PerspectiveScalePolicy::Preserve;
			auto const result = SolvePerspectiveTags(input);
			ASSERT_TRUE(result) << DescribeSolverError(result.error);
			ASSERT_TRUE(result.candidate);
			EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
			EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
			auto const landed = ForwardQuad(source, result.candidate->state);
			ASSERT_TRUE(landed);
			auto const& quad = landed.quad;
			// Opposite edges must remain parallel and adjacent edges orthogonal.
			Vec2 const top = quad[1] - quad[0];
			Vec2 const left = quad[3] - quad[0];
			Vec2 const target_top = input.target[1] - input.target[0];
			EXPECT_LE(std::abs(top.Cross(target_top)),
					  1.0e-5 * std::sqrt(top.SquareLength() * target_top.SquareLength()));
			EXPECT_GT(top.x * target_top.x + top.y * target_top.y, 0.0);
			EXPECT_LE(std::sqrt((top - (quad[2] - quad[3])).SquareLength()),
					  2.0 * input.max_error);
			EXPECT_LE(std::sqrt((left - (quad[2] - quad[1])).SquareLength()),
					  2.0 * input.max_error);
			EXPECT_LE(std::abs(top.x * left.x + top.y * left.y),
					  1.0e-5 * std::sqrt(top.SquareLength() * left.SquareLength()));
			Vec2 const center = (quad[0] + quad[2]) / 2.0;
			EXPECT_NEAR(desired.position.x, center.x, input.max_error);
			EXPECT_NEAR(desired.position.y, center.y, input.max_error);
		}
	}
}

TEST(perspective_tag_solver, preserve_scale_flattens_an_existing_plane_for_a_rectangle) {
	auto source = BaseInput();
	source.state.alignment = 5;
	source.state.origin = Vec2{.x = 410.0, .y = 290.0};
	source.state.rotation_x = 15.0;
	source.state.rotation_y = -8.0;
	source.state.rotation_z = -90.0;
	SolverInput input{.source = source,
					  .target = MakeQuad({.left = 456.0, .top = 223.0, .right = 544.0, .bottom = 377.0})};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	auto const landed = ForwardQuad(source, result.candidate->state);
	ASSERT_TRUE(landed);
	EXPECT_NEAR(landed.quad[0].y, landed.quad[1].y, 1.0e-6);
	EXPECT_NEAR(landed.quad[2].y, landed.quad[3].y, 1.0e-6);
	EXPECT_NEAR(landed.quad[0].x, landed.quad[3].x, 1.0e-6);
	EXPECT_NEAR(landed.quad[1].x, landed.quad[2].x, 1.0e-6);
}

TEST(perspective_tag_solver, preserve_scale_keeps_a_locked_rectangle_rectangular) {
	auto source = BaseInput();
	source.state.alignment = 5;
	SolverInput input{
		source,
		Quad{{
			{450.0, 100.0},
			{450.0, 300.0},
			{400.0, 300.0},
			{400.0, 100.0},
		}}};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.locked_rotation_z = 270.0;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	auto const landed = ForwardQuad(source, result.candidate->state);
	ASSERT_TRUE(landed);
	EXPECT_NEAR(landed.quad[0].x, landed.quad[1].x, 1.0e-6);
	EXPECT_NEAR(landed.quad[2].x, landed.quad[3].x, 1.0e-6);
	EXPECT_NEAR(landed.quad[0].y, landed.quad[3].y, 1.0e-6);
	EXPECT_NEAR(landed.quad[1].y, landed.quad[2].y, 1.0e-6);
}

TEST(perspective_tag_solver, preserve_scale_keeps_a_parallelogram_similar) {
	auto source = BaseInput();
	source.state.scale_x = 120.0;
	source.state.scale_y = 80.0;
	auto desired = source.state;
	desired.position = {535.0, 405.0};
	desired.scale_x = 156.0;
	desired.scale_y = 104.0;
	desired.shear_x = 0.18;
	desired.shear_y = 0.07;
	desired.rotation_z = 19.0;

	SolverInput input{source, TargetFrom(source, desired)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::PreserveShapeAffine, result.candidate->family)
		<< static_cast<int>(result.candidate->family);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);

	auto landed = source;
	landed.state = result.candidate->state;
	auto const forward = ForwardQuad(landed);
	ASSERT_TRUE(forward);
	Vec2 center;
	for (auto const& point : input.target)
		center = center + point / 4.0;
	double numerator = 0.0;
	double denominator = 0.0;
	for (std::size_t index = 0; index < input.target.size(); ++index) {
		Vec2 const target_vector = input.target[index] - center;
		Vec2 const landed_vector = forward.quad[index] - center;
		numerator += target_vector.Dot(landed_vector);
		denominator += target_vector.SquareLength();
	}
	ASSERT_GT(denominator, 0.0);
	double const factor = numerator / denominator;
	ASSERT_GT(factor, 0.0);
	for (std::size_t index = 0; index < input.target.size(); ++index) {
		Vec2 const expected = center + (input.target[index] - center) * factor;
		EXPECT_NEAR(expected.x, forward.quad[index].x, 2.0 * input.max_error)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, forward.quad[index].y, 2.0 * input.max_error)
			<< "corner " << index;
	}
}

TEST(perspective_tag_solver, preserve_scale_keeps_a_locked_rotation_parallelogram_similar) {
	auto source = BaseInput();
	source.state.scale_x = 120.0;
	source.state.scale_y = 80.0;
	source.state.rotation_z = 270.0;
	auto desired = source.state;
	desired.position = {535.0, 405.0};
	desired.scale_x = 156.0;
	desired.scale_y = 104.0;
	desired.shear_x = 0.18;
	desired.shear_y = 0.07;

	SolverInput input{source, TargetFrom(source, desired)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.locked_rotation_z = 270.0;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(270.0, result.candidate->state.rotation_z);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);

	auto landed = source;
	landed.state = result.candidate->state;
	auto const forward = ForwardQuad(landed);
	ASSERT_TRUE(forward);
	Vec2 center;
	for (auto const& point : input.target)
		center = center + point / 4.0;
	double numerator = 0.0;
	double denominator = 0.0;
	for (std::size_t index = 0; index < input.target.size(); ++index) {
		Vec2 const target_vector = input.target[index] - center;
		Vec2 const landed_vector = forward.quad[index] - center;
		numerator += target_vector.Dot(landed_vector);
		denominator += target_vector.SquareLength();
	}
	ASSERT_GT(denominator, 0.0);
	double const factor = numerator / denominator;
	ASSERT_GT(factor, 0.0);
	for (std::size_t index = 0; index < input.target.size(); ++index) {
		Vec2 const expected = center + (input.target[index] - center) * factor;
		EXPECT_NEAR(expected.x, forward.quad[index].x, 2.0 * input.max_error)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, forward.quad[index].y, 2.0 * input.max_error)
			<< "corner " << index;
	}
}

TEST(perspective_tag_solver, preserve_scale_keeps_an_extreme_trapezoid_similar) {
	auto source = BaseInput();
	source.bounds.rectangle = {0.0, 0.0, 38.0, 42.0};
	SolverInput input{
		source,
		Quad{{
			{160.0, 239.0},
			{872.0, 105.0},
			{551.0, 553.0},
			{174.0, 553.0},
		}}};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.max_error = 0.1;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);

	auto landed = source;
	landed.state = result.candidate->state;
	auto const forward = ForwardQuad(landed);
	ASSERT_TRUE(forward);
	Vec2 center;
	for (auto const& point : input.target)
		center = center + point / 4.0;
	double numerator = 0.0;
	double denominator = 0.0;
	for (std::size_t index = 0; index < input.target.size(); ++index) {
		Vec2 const target_vector = input.target[index] - center;
		Vec2 const landed_vector = forward.quad[index] - center;
		numerator += target_vector.Dot(landed_vector);
		denominator += target_vector.SquareLength();
	}
	ASSERT_GT(denominator, 0.0);
	double const factor = numerator / denominator;
	ASSERT_GT(factor, 0.0);
	for (std::size_t index = 0; index < input.target.size(); ++index) {
		Vec2 const expected = center + (input.target[index] - center) * factor;
		EXPECT_NEAR(expected.x, forward.quad[index].x, 0.2)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, forward.quad[index].y, 0.2)
			<< "corner " << index;
	}
}

TEST(perspective_tag_solver, preserve_scale_keeps_non_integer_style_scale_exact) {
	auto source = BaseInput();
	source.state.scale_x = 123.456789;
	source.state.scale_y = 87.654321;
	auto target = source.state;
	target.position = {535.0, 405.0};
	SolverInput input{source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_EQ(1, result.candidate->score.changed_tag_count);
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, preserve_scale_keeps_projective_candidate_scales_fixed) {
	auto source = BaseInput();
	source.state.scale_x = 121.234567;
	source.state.scale_y = 88.765432;
	auto target = source.state;
	target.position = {560.0, 390.0};
	target.shear_x = 0.16;
	target.rotation_x = 21.0;
	target.rotation_y = -17.0;
	target.rotation_z = 13.0;
	SolverInput input{source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto fit_input = input;
	fit_input.scale_policy = PerspectiveScalePolicy::Fit;
	auto const fit = SolvePerspectiveTags(fit_input);
	ASSERT_TRUE(fit) << DescribeSolverError(fit.error);
	ASSERT_TRUE(fit.candidate);

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::PreserveFitState, result.candidate->family);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_DOUBLE_EQ(fit.candidate->state.position.x, result.candidate->state.position.x);
	EXPECT_DOUBLE_EQ(fit.candidate->state.position.y, result.candidate->state.position.y);
	ASSERT_EQ(fit.candidate->state.origin.has_value(), result.candidate->state.origin.has_value());
	if (fit.candidate->state.origin)
		EXPECT_DOUBLE_EQ(fit.candidate->state.origin->x, result.candidate->state.origin->x);
	if (fit.candidate->state.origin)
		EXPECT_DOUBLE_EQ(fit.candidate->state.origin->y, result.candidate->state.origin->y);
	EXPECT_DOUBLE_EQ(fit.candidate->state.shear_x, result.candidate->state.shear_x);
	EXPECT_DOUBLE_EQ(fit.candidate->state.shear_y, result.candidate->state.shear_y);
	EXPECT_DOUBLE_EQ(fit.candidate->state.rotation_x, result.candidate->state.rotation_x);
	EXPECT_DOUBLE_EQ(fit.candidate->state.rotation_y, result.candidate->state.rotation_y);
	EXPECT_DOUBLE_EQ(fit.candidate->state.rotation_z, result.candidate->state.rotation_z);
	EXPECT_GT(result.candidate->snap_error, input.max_error);
}

TEST(perspective_tag_solver, equivalent_locked_rotation_keeps_affine_representation) {
	auto source = BaseInput();
	source.state.rotation_z = 270.0;
	auto desired = source.state;
	desired.position = {520.0, 410.0};
	desired.scale_x = 125.0;
	desired.scale_y = 90.0;
	desired.shear_x = 0.23;
	SolverInput input {source, TargetFrom(source, desired)};
	input.locked_rotation_z = 270.0;

	auto const solved = SolvePerspectiveTags(input);
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_NE(CandidateFamily::ProjectiveExplicitOrigin, solved.candidate->family);
	EXPECT_FALSE(solved.candidate->state.origin);
	EXPECT_EQ("270", solved.candidate->serialized.rotation_z);
	EXPECT_LE(solved.candidate->max_error, input.max_error);

	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const constrained = SolvePerspectiveTags(input);
	ASSERT_TRUE(constrained) << DescribeSolverError(constrained.error);
	ASSERT_TRUE(constrained.candidate);
	EXPECT_EQ(CandidateFamily::AffineFax, constrained.candidate->family);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		constrained.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	EXPECT_DOUBLE_EQ(270.0, constrained.candidate->state.rotation_z);
	EXPECT_GT(std::abs(constrained.candidate->state.shear_x), 1.0e-4);
}

TEST(perspective_tag_solver, locked_rotation_uses_no_origin_double_shear_projective_candidate) {
	auto source = BaseInput();
	source.state.rotation_z = 270.0;
	auto desired = source.state;
	desired.position = {525.0, 405.0};
	desired.scale_x = 121.0;
	desired.scale_y = 88.0;
	desired.shear_x = 0.17;
	desired.shear_y = -0.11;
	desired.rotation_x = 18.0;
	desired.rotation_y = -13.0;

	SolverInput input {source, TargetFrom(source, desired)};
	input.output_mapping = {1.25, 0.75};
	input.locked_rotation_z = 270.0;
	auto const solved = SolvePerspectiveTags(input);

	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_EQ(CandidateFamily::ProjectiveImplicitLockedDoubleShear,
		solved.candidate->family);
	EXPECT_FALSE(solved.candidate->state.origin);
	EXPECT_NEAR(270.0, solved.candidate->state.rotation_z, 1.0e-12);
	EXPECT_EQ("270", solved.candidate->serialized.rotation_z);
	EXPECT_GT(std::abs(solved.candidate->state.shear_x), 1.0e-4);
	EXPECT_GT(std::abs(solved.candidate->state.shear_y), 1.0e-4);
	EXPECT_EQ(0, solved.candidate->score.explicit_origin_penalty);
	EXPECT_LE(solved.candidate->max_error, input.max_error);

	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const constrained = SolvePerspectiveTags(input);
	ASSERT_TRUE(constrained) << DescribeSolverError(constrained.error);
	ASSERT_TRUE(constrained.candidate);
	// The switch has no projective family, so the locked plane settles for the
	// nearest clean affine with the rotation still pinned.
	EXPECT_DOUBLE_EQ(270.0, constrained.candidate->state.rotation_z);
	EXPECT_TRUE(constrained.candidate->Snapped());
}

TEST(perspective_tag_solver, locked_double_shear_handles_equivalent_angles_and_alignment_metrics) {
	std::array<double, 3> const rotations {90.0, -90.0, 270.0};
	for (double const rotation : rotations) {
		auto source = BaseInput();
		source.bounds.alignment_extent = Resolution {240.0, 120.0};
		source.bounds.alignment_offset = {8.0, -5.0};
		source.state.alignment = 5;
		source.state.rotation_z = rotation;
		auto desired = source.state;
		desired.position = {530.0, 400.0};
		desired.scale_x = 116.0;
		desired.scale_y = 93.0;
		desired.shear_x = 0.14;
		desired.shear_y = -0.09;
		desired.rotation_x = 15.0;
		desired.rotation_y = -10.0;

		SolverInput input {source, TargetFrom(source, desired)};
		input.locked_rotation_z = rotation;
		auto const solved = SolvePerspectiveTags(input);

		ASSERT_TRUE(solved)
			<< "frz=" << rotation << ": " << DescribeSolverError(solved.error);
		ASSERT_TRUE(solved.candidate);
		EXPECT_EQ(CandidateFamily::ProjectiveImplicitLockedDoubleShear,
			solved.candidate->family);
		EXPECT_FALSE(solved.candidate->state.origin);
		EXPECT_DOUBLE_EQ(rotation, solved.candidate->state.rotation_z);
		EXPECT_EQ(FormatAssNumber(rotation, 5),
			solved.candidate->serialized.rotation_z);
		EXPECT_LE(solved.candidate->max_error, input.max_error);
	}
}

TEST(perspective_tag_solver, drawing_candidate_uses_renderer_alignment_metrics) {
	auto source = BaseInput();
	source.bounds = {
		{10.0, 20.0, 110.0, 70.0}, BoundsKind::Drawing,
		Resolution {100.0, 62.0}, Vec2 {0.0, 25.0}};
	source.state.alignment = 5;
	auto target = source.state;
	target.position = {525.0, 415.0};
	target.scale_x = 132.0;
	target.scale_y = 132.0;
	target.rotation_z = 23.0;
	auto const result = SolveTo(source, target);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::Similarity, result.candidate->family);
	EXPECT_LE(result.candidate->max_error, 0.1);
}

TEST(perspective_tag_solver, affine_candidates_choose_a_single_canonical_shear) {
	auto source = BaseInput();
	auto fax_target = source.state;
	fax_target.scale_x = 125.0;
	fax_target.scale_y = 78.0;
	fax_target.shear_x = 0.32;
	fax_target.rotation_z = -14.0;
	auto fax_result = SolveTo(source, fax_target);
	ASSERT_TRUE(fax_result) << DescribeSolverError(fax_result.error);
	ASSERT_TRUE(fax_result.candidate);
	EXPECT_EQ(CandidateFamily::AffineFax, fax_result.candidate->family);
	EXPECT_NEAR(0.0, fax_result.candidate->state.shear_y, 1.0e-9);
	EXPECT_NEAR(0.0, fax_result.candidate->state.rotation_x, 1.0e-9);
	EXPECT_NEAR(0.0, fax_result.candidate->state.rotation_y, 1.0e-9);

	auto fay_target = source.state;
	fay_target.scale_x = 82.0;
	fay_target.scale_y = 142.0;
	fay_target.shear_y = -0.27;
	fay_target.rotation_z = 19.0;
	auto fay_result = SolveTo(source, fay_target);
	ASSERT_TRUE(fay_result) << DescribeSolverError(fay_result.error);
	ASSERT_TRUE(fay_result.candidate);
	EXPECT_EQ(CandidateFamily::AffineFax, fay_result.candidate->family);
	EXPECT_NEAR(0.0, fay_result.candidate->state.shear_y, 1.0e-9);
	EXPECT_LE(fay_result.candidate->max_error, 0.1);
}

TEST(perspective_tag_solver, approximate_affine_is_used_before_projective_tags) {
	auto source = BaseInput();
	auto affine_state = source.state;
	affine_state.position = {525.0, 405.0};
	affine_state.scale_x = 125.0;
	affine_state.scale_y = 82.0;
	affine_state.shear_x = 0.24;
	affine_state.rotation_z = -13.0;
	auto target = TargetFrom(source, affine_state);
	target[0].x += 0.2;

	SolverInput loose {source, target};
	loose.max_error = 0.1;
	auto const affine = SolvePerspectiveTags(loose);
	ASSERT_TRUE(affine) << DescribeSolverError(affine.error);
	ASSERT_TRUE(affine.candidate);
	EXPECT_EQ(CandidateFamily::AffineFax, affine.candidate->family);
	EXPECT_FALSE(affine.candidate->state.origin);
	EXPECT_NEAR(0.0, affine.candidate->state.shear_y, 1.0e-9);
	EXPECT_NEAR(0.0, affine.candidate->state.rotation_x, 1.0e-9);
	EXPECT_NEAR(0.0, affine.candidate->state.rotation_y, 1.0e-9);
	EXPECT_LE(affine.candidate->max_error, loose.max_error);

	loose.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const constrained_affine = SolvePerspectiveTags(loose);
	ASSERT_TRUE(constrained_affine)
		<< DescribeSolverError(constrained_affine.error);
	ASSERT_TRUE(constrained_affine.candidate);
	EXPECT_EQ(CandidateFamily::AffineFax,
		constrained_affine.candidate->family);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		constrained_affine.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));

	SolverInput strict {source, target};
	strict.max_error = 0.01;
	auto const projective = SolvePerspectiveTags(strict);
	ASSERT_TRUE(projective) << DescribeSolverError(projective.error);
	ASSERT_TRUE(projective.candidate);
	EXPECT_TRUE(
		projective.candidate->family == CandidateFamily::ProjectiveImplicitFax
		|| projective.candidate->family == CandidateFamily::ProjectiveImplicitFay
		|| projective.candidate->family == CandidateFamily::ProjectiveExplicitOrigin);
	EXPECT_LE(projective.candidate->max_error, strict.max_error);

	strict.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const constrained_projective = SolvePerspectiveTags(strict);
	ASSERT_TRUE(constrained_projective)
		<< DescribeSolverError(constrained_projective.error);
	ASSERT_TRUE(constrained_projective.candidate);
	// The exact projective fit is not expressible in the restricted subset, so
	// the nearest affine is settled for with the shortfall reported.
	EXPECT_TRUE(constrained_projective.candidate->Snapped());
}

TEST(perspective_tag_solver, projective_candidate_uses_implicit_origin_when_feasible) {
	auto source = BaseInput();
	auto target = source.state;
	target.position = {560.0, 390.0};
	target.scale_x = 118.0;
	target.scale_y = 92.0;
	target.shear_x = 0.16;
	target.rotation_x = 21.0;
	target.rotation_y = -17.0;
	target.rotation_z = 13.0;
	auto const result = SolveTo(source, target);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_TRUE(
		result.candidate->family == CandidateFamily::ProjectiveImplicitFax
		|| result.candidate->family == CandidateFamily::ProjectiveImplicitFay);
	EXPECT_FALSE(result.candidate->state.origin);
	EXPECT_LE(result.candidate->max_error, 0.1);
	EXPECT_EQ(0, result.candidate->score.explicit_origin_penalty);
}

TEST(perspective_tag_solver, solves_a_hand_drawn_non_parallel_target_quad) {
	SolverInput input;
	input.source = BaseInput();
	input.target = Quad {{
		{380.0, 280.0},
		{660.0, 300.0},
		{620.0, 460.0},
		{410.0, 430.0},
	}};

	auto const solved = SolvePerspectiveTags(input);
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_TRUE(
		solved.candidate->family == CandidateFamily::ProjectiveImplicitFax
		|| solved.candidate->family == CandidateFamily::ProjectiveImplicitFay
		|| solved.candidate->family == CandidateFamily::ProjectiveExplicitOrigin);
	EXPECT_LE(solved.candidate->max_error, input.max_error);

	auto candidate = input.source;
	candidate.state = solved.candidate->state;
	auto const measured = MeasurePerspectiveResidual(
		candidate, input.target, input.output_mapping);
	ASSERT_TRUE(measured) << DescribeResidualError(measured.error);
	EXPECT_LE(measured.max_error, input.max_error);
}

TEST(perspective_tag_solver, apply_result_is_deterministic_and_quantized) {
	auto source = BaseInput();
	auto target = source.state;
	target.position = {512.34567, 401.23456};
	target.scale_x = 123.4567;
	target.scale_y = 87.6543;
	target.shear_x = 0.234567;
	target.rotation_x = 9.87654;
	target.rotation_y = -7.65432;
	target.rotation_z = 21.23456;
	auto const first = SolveTo(source, target);
	auto const second = SolveTo(source, target);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	ASSERT_TRUE(first.candidate);
	ASSERT_TRUE(second.candidate);
	EXPECT_EQ(first.candidate->family, second.candidate->family);
	EXPECT_EQ(first.candidate->serialized.position, second.candidate->serialized.position);
	EXPECT_EQ(first.candidate->serialized.scale_x, second.candidate->serialized.scale_x);
	EXPECT_EQ(first.candidate->serialized.rotation_x, second.candidate->serialized.rotation_x);
	EXPECT_DOUBLE_EQ(first.candidate->max_error, second.candidate->max_error);
}

TEST(perspective_tag_solver, serialized_tags_honor_maximum_decimals) {
	auto source = BaseInput();
	auto target = source.state;
	target.position = {512.34567, 401.23456};
	target.scale_x = 123.45;
	target.scale_y = 87.65;
	target.rotation_z = 21.23456;

	SolverInput input {source, TargetFrom(source, target)};
	input.maximum_decimals = 2;
	auto const two = SolvePerspectiveTags(input);
	ASSERT_TRUE(two) << DescribeSolverError(two.error);
	ASSERT_TRUE(two.candidate);
	ExpectSerializedAtMost(two.candidate->serialized, 2);
	EXPECT_LE(FractionalDigits(two.candidate->serialized.rotation_z), 2);

	auto integer_target = source.state;
	integer_target.position = {520.0, 400.0};
	integer_target.scale_x = 125.0;
	integer_target.scale_y = 80.0;
	integer_target.rotation_z = 15.0;
	SolverInput integer_input {source, TargetFrom(source, integer_target)};
	integer_input.maximum_decimals = 0;
	auto const integers = SolvePerspectiveTags(integer_input);
	ASSERT_TRUE(integers) << DescribeSolverError(integers.error);
	ASSERT_TRUE(integers.candidate);
	ExpectSerializedAtMost(integers.candidate->serialized, 0);
}

TEST(perspective_tag_solver, randomized_forward_inverse_stays_inside_output_error_budget) {
	std::mt19937 generator(0x5EEDu);
	std::uniform_real_distribution<double> position_x(280.0, 720.0);
	std::uniform_real_distribution<double> position_y(180.0, 520.0);
	std::uniform_real_distribution<double> scale(70.0, 155.0);
	std::uniform_real_distribution<double> shear(-0.3, 0.3);
	std::uniform_real_distribution<double> tilt(-24.0, 24.0);
	std::uniform_real_distribution<double> rotation(-35.0, 35.0);

	auto source = BaseInput();
	source.state.outline_x = 3.0;
	source.state.outline_y = 2.0;
	source.state.shadow_x = 4.0;
	source.state.shadow_y = -3.0;
	int accepted = 0;
	for (int attempt = 0; attempt < 80 && accepted < 24; ++attempt) {
		auto target = source.state;
		target.position = {position_x(generator), position_y(generator)};
		target.scale_x = scale(generator);
		target.scale_y = scale(generator);
		target.shear_x = shear(generator);
		target.rotation_x = tilt(generator);
		target.rotation_y = tilt(generator);
		target.rotation_z = rotation(generator);
		auto const forward = ForwardQuad(ForwardInput {
			source.play_resolution,
			source.layout_resolution,
			source.video_storage_resolution,
			source.bounds,
			target,
		});
		if (!forward)
			continue;

		SolverInput input;
		input.source = source;
		input.target = forward.quad;
		input.output_mapping = {1.25, 0.75};
		input.max_error = 0.1;
		auto const result = SolvePerspectiveTags(input);
		ASSERT_TRUE(result) << "attempt " << attempt << ": " << DescribeSolverError(result.error);
		ASSERT_TRUE(result.candidate);
		EXPECT_LE(result.candidate->max_error, input.max_error);
		++accepted;
	}
	EXPECT_EQ(24, accepted);
}

// The same randomized probe at the shipped UI default of four decimal places
// rather than the solver's six-digit ceiling: two digits used to be the
// default and routinely starved the 0.1 px budget on projective states, which
// is what motivated raising it. Every random projective quad must solve
// inside the budget at four digits, and the emitted tags must respect the cap
// while still using the shortest representation that fits.
TEST(perspective_tag_solver, randomized_projective_solves_at_the_ui_default_precision) {
	std::mt19937 generator(0x5EEDu);
	std::uniform_real_distribution<double> position_x(280.0, 720.0);
	std::uniform_real_distribution<double> position_y(180.0, 520.0);
	std::uniform_real_distribution<double> scale(70.0, 155.0);
	std::uniform_real_distribution<double> shear(-0.3, 0.3);
	std::uniform_real_distribution<double> tilt(-24.0, 24.0);
	std::uniform_real_distribution<double> rotation(-35.0, 35.0);

	auto source = BaseInput();
	int accepted = 0;
	for (int attempt = 0; attempt < 80 && accepted < 24; ++attempt) {
		auto target = source.state;
		target.position = {position_x(generator), position_y(generator)};
		target.scale_x = scale(generator);
		target.scale_y = scale(generator);
		target.shear_x = shear(generator);
		target.rotation_x = tilt(generator);
		target.rotation_y = tilt(generator);
		target.rotation_z = rotation(generator);
		auto const forward = ForwardQuad(ForwardInput {
			source.play_resolution,
			source.layout_resolution,
			source.video_storage_resolution,
			source.bounds,
			target,
		});
		if (!forward)
			continue;

		SolverInput input;
		input.source = source;
		input.target = forward.quad;
		input.output_mapping = {1.25, 0.75};
		input.max_error = 0.1;
		input.maximum_decimals = kDefaultPerspectiveDecimalPlaces;
		auto const result = SolvePerspectiveTags(input);
		ASSERT_TRUE(result) << "attempt " << attempt << ": "
			<< DescribeSolverError(result.error);
		ASSERT_TRUE(result.candidate);
		EXPECT_LE(result.candidate->max_error, input.max_error);
		ExpectSerializedAtMost(
			result.candidate->serialized, kDefaultPerspectiveDecimalPlaces);
		++accepted;
	}
	EXPECT_EQ(24, accepted);
}

TEST(perspective_tag_solver, invalid_target_and_output_mapping_fail_without_candidate) {
	auto source = BaseInput();
	auto target = TargetFrom(source, source.state);
	std::swap(target[1], target[3]);
	SolverInput input {source, target};
	auto invalid_target = SolvePerspectiveTags(input);
	EXPECT_EQ(SolverError::InvalidTarget, invalid_target.error);
	EXPECT_FALSE(invalid_target.candidate);

	input.target = TargetFrom(source, source.state);
	input.output_mapping.scale_x = 0.0;
	auto invalid_mapping = SolvePerspectiveTags(input);
	EXPECT_EQ(SolverError::InvalidOutputMapping, invalid_mapping.error);
	EXPECT_FALSE(invalid_mapping.candidate);

	input.output_mapping.scale_x = 1.0;
	input.locked_rotation_z = std::numeric_limits<double>::infinity();
	auto invalid_rotation_lock = SolvePerspectiveTags(input);
	EXPECT_EQ(SolverError::InvalidSource, invalid_rotation_lock.error);
	EXPECT_EQ(ForwardError::NonFiniteState, invalid_rotation_lock.forward_error);
	EXPECT_FALSE(invalid_rotation_lock.candidate);

	input.locked_rotation_z = MaxTransformParameter + 1.0;
	auto out_of_range_rotation_lock = SolvePerspectiveTags(input);
	EXPECT_EQ(SolverError::InvalidSource, out_of_range_rotation_lock.error);
	EXPECT_EQ(ForwardError::TransformParameterOutOfRange,
		out_of_range_rotation_lock.forward_error);
	EXPECT_FALSE(out_of_range_rotation_lock.candidate);
}

TEST(perspective_tag_solver, user_target_can_replace_an_unprojectable_current_transform) {
	auto source = BaseInput();
	auto desired = source.state;
	desired.position = {520.0, 410.0};
	desired.scale_x = 125.0;
	desired.scale_y = 90.0;
	auto const target = TargetFrom(source, desired);

	source.state.scale_y = 1000.0;
	source.state.rotation_x = 90.0;
	auto const current = ForwardQuad(source);
	ASSERT_FALSE(current);
	EXPECT_EQ(ForwardError::ProjectionDomain, current.error);

	SolverInput input {source, target};
	auto const solved = SolvePerspectiveTags(input);
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_NE(CandidateFamily::NoOp, solved.candidate->family);
	EXPECT_NE(CandidateFamily::CurrentRepresentation, solved.candidate->family);
	EXPECT_LE(solved.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, projective_fallbacks_survive_an_unprojectable_current_transform) {
	auto source = BaseInput();
	auto desired = source.state;
	desired.position = {560.0, 390.0};
	desired.scale_x = 118.0;
	desired.scale_y = 92.0;
	desired.shear_x = 0.16;
	desired.rotation_x = 21.0;
	desired.rotation_y = -17.0;
	desired.rotation_z = 13.0;
	auto const target = TargetFrom(source, desired);

	source.state.scale_y = 1000.0;
	source.state.rotation_x = 90.0;
	ASSERT_EQ(ForwardError::ProjectionDomain, ForwardQuad(source).error);
	auto const solved = SolvePerspectiveTags({source, target});
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_TRUE(
		solved.candidate->family == CandidateFamily::ProjectiveImplicitFax
		|| solved.candidate->family == CandidateFamily::ProjectiveImplicitFay
		|| solved.candidate->family == CandidateFamily::ProjectiveExplicitOrigin);
	EXPECT_GE(solved.considered_candidates, 3u);
}

TEST(perspective_tag_solver, user_target_can_replace_an_out_of_range_current_rotation) {
	auto source = BaseInput();
	auto desired = source.state;
	desired.position = {535.0, 405.0};
	desired.scale_x = 112.0;
	desired.scale_y = 94.0;
	auto const target = TargetFrom(source, desired);

	source.state.rotation_x = 1.0e8;
	ASSERT_EQ(ForwardError::TransformParameterOutOfRange, ForwardQuad(source).error);
	auto const solved = SolvePerspectiveTags({source, target});
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);
	EXPECT_NE(CandidateFamily::NoOp, solved.candidate->family);
	EXPECT_NE(CandidateFamily::CurrentRepresentation, solved.candidate->family);
	EXPECT_LE(solved.candidate->max_error, 0.1);
}

TEST(perspective_tag_solver, structural_source_errors_still_fail_before_solving) {
	auto source = BaseInput();
	auto const target = TargetFrom(source, source.state);
	source.play_resolution.width = 0.0;
	auto const solved = SolvePerspectiveTags({source, target});
	EXPECT_EQ(SolverError::InvalidSource, solved.error);
	EXPECT_EQ(ForwardError::InvalidResolution, solved.forward_error);
	EXPECT_FALSE(solved.candidate);

	for (auto const member : {
			&EvaluatedTransformState::outline_x,
			&EvaluatedTransformState::shadow_y}) {
		source = BaseInput();
		source.state.*member = 1.0e8;
		auto const invalid_decoration = SolvePerspectiveTags({source, target});
		EXPECT_EQ(SolverError::InvalidSource, invalid_decoration.error);
		EXPECT_EQ(ForwardError::TransformParameterOutOfRange,
			invalid_decoration.forward_error);
		EXPECT_FALSE(invalid_decoration.candidate);
	}

	source = BaseInput();
	source.bounds.residual_samples = {
		{std::numeric_limits<double>::quiet_NaN(), 0.0}};
	auto const invalid_sample = SolvePerspectiveTags({source, target});
	EXPECT_EQ(SolverError::InvalidSource, invalid_sample.error);
	EXPECT_EQ(ForwardError::InvalidBounds, invalid_sample.forward_error);
	EXPECT_FALSE(invalid_sample.candidate);
}

TEST(perspective_tag_solver, strict_budget_rejects_all_quantized_candidates_without_output) {
	auto source = BaseInput();
	auto target_state = source.state;
	target_state.position = {512.3456789, 401.2345678};

	SolverInput input;
	input.source = source;
	input.target = TargetFrom(source, target_state);
	input.max_error = 1.0e-8;
	auto const result = SolvePerspectiveTags(input);

	EXPECT_EQ(SolverError::NoFeasibleCandidate, result.error);
	EXPECT_FALSE(result.candidate);
	EXPECT_GT(result.considered_candidates, 0u);
	EXPECT_STRNE("unknown Perspective solver error", DescribeSolverError(result.error));
}

// Rounding, not reachability: the projective families model this quad almost
// exactly, but at two decimal places the shear digits alone move the box by
// several pixels, so every quantized candidate dies after the model stage.
// The same model written with four decimals fits easily: this is the one
// refusal reason left that names an option, and the old flag-based UI guess
// could never tell it apart and never blamed rounding at all.
TEST(perspective_tag_solver, coarse_rounding_settles_far_away_until_decimals_allow_the_fit) {
	auto source = BaseInput();
	auto target_state = source.state;
	target_state.position = {512.34567, 401.23456};
	target_state.scale_x = 123.4567;
	target_state.scale_y = 87.6543;
	target_state.shear_x = 0.234567;
	target_state.rotation_x = 9.87654;
	target_state.rotation_y = -7.65432;
	target_state.rotation_z = 21.23456;
	auto const target = TargetFrom(source, target_state);

	SolverInput input {source, target};
	// The output mapping doubles every rounding error, so the third digit of
	// the shear alone costs more than the whole 0.1 px budget.
	input.output_mapping = {2.0, 2.0};
	input.maximum_decimals = 2;
	auto const coarse = SolvePerspectiveTags(input);
	ASSERT_TRUE(coarse) << DescribeSolverError(coarse.error);
	ASSERT_TRUE(coarse.candidate);
	// With every real family's digits over the rounding budget at two
	// decimals, only the weak translation-level fits survive: the solve still
	// returns the best representable candidate, but it lands far from the
	// target and says so in snap_error -- the preview draws the gap.
	EXPECT_TRUE(coarse.candidate->Snapped());
	EXPECT_GT(coarse.candidate->snap_error, 100.0);

	// The degradation is about the digits, not the shape: the same target
	// solves exactly once the tags are allowed four decimals.
	input.maximum_decimals = 4;
	auto const fine = SolvePerspectiveTags(input);
	ASSERT_TRUE(fine) << DescribeSolverError(fine.error);
	ASSERT_TRUE(fine.candidate);
	EXPECT_FALSE(fine.candidate->Snapped());
	EXPECT_LE(fine.candidate->max_error, input.max_error);
	ExpectSerializedAtMost(fine.candidate->serialized, 4);
}

// A line whose only restricted tag is \fay, aimed at a quad no affine family
// can reach. Zeroing the \fay is policy-legal -- dropping a restricted tag
// introduces nothing -- so the clean affine families run and settle for the
// nearest flat fit. Contrast the org-bearing line above, where the filter
// leaves only the declared-plane refit.

TEST(perspective_tag_solver, restricted_policy_settles_for_the_nearest_clean_affine) {
	auto source = BaseInput();
	source.state.shear_y = 0.1;
	EvaluatedTransformState const projective = [] {
		auto state = EvaluatedTransformState {};
		state.alignment = 7;
		state.position = {505.0, 345.0};
		state.scale_x = 118.0;
		state.scale_y = 92.0;
		state.rotation_x = 15.0;
		state.rotation_y = -9.0;
		state.rotation_z = 6.0;
		return state;
	}();
	auto const target = TargetFrom(source, projective);

	SolverInput input {source, target};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const restricted = SolvePerspectiveTags(input);
	ASSERT_TRUE(restricted) << DescribeSolverError(restricted.error);
	ASSERT_TRUE(restricted.candidate);
	// Zeroing the fay tag is policy-legal, so the clean affine families run and
	// the nearest one is settled for with the shortfall reported, not a
	// refusal: under the switch a projective quad gets the closest flat fit.
	EXPECT_TRUE(restricted.candidate->Snapped());
	EXPECT_GT(restricted.candidate->snap_error, input.max_error);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		restricted.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));

	// Not a compromise the switch cannot escape: unrestricted, the projective
	// families solve the same quad outright.
	input.representation_policy = PerspectiveRepresentationPolicy::Automatic;
	auto const automatic = SolvePerspectiveTags(input);
	ASSERT_TRUE(automatic) << DescribeSolverError(automatic.error);
	ASSERT_TRUE(automatic.candidate);
	EXPECT_FALSE(automatic.candidate->Snapped());
}

TEST(perspective_tag_solver, public_residual_verifier_matches_quantized_candidate) {
	auto source = BaseInput();
	source.state.outline_x = 2.0;
	source.state.outline_y = 3.0;
	source.state.shadow_x = 4.0;
	source.state.shadow_y = -2.0;
	auto target_state = source.state;
	target_state.position = {523.4567, 412.3456};
	target_state.scale_x = 128.7654;
	target_state.scale_y = 91.2345;
	target_state.shear_x = 0.214567;
	target_state.rotation_x = 11.23456;
	target_state.rotation_y = -8.76543;
	target_state.rotation_z = 19.87654;

	SolverInput input;
	input.source = source;
	input.target = TargetFrom(source, target_state);
	input.output_mapping = {1.25, 0.75};
	auto const solved = SolvePerspectiveTags(input);
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);
	ASSERT_TRUE(solved.candidate);

	auto candidate = source;
	candidate.state = solved.candidate->state;
	auto const measured = MeasurePerspectiveResidual(
		candidate, input.target, input.output_mapping);
	ASSERT_TRUE(measured) << DescribeResidualError(measured.error);
	EXPECT_DOUBLE_EQ(solved.candidate->max_error, measured.max_error);

	auto invalid_candidate = candidate;
	invalid_candidate.state.scale_x = 0.0;
	auto const bad_candidate = MeasurePerspectiveResidual(
		invalid_candidate, input.target, input.output_mapping);
	EXPECT_EQ(ResidualError::InvalidCandidate, bad_candidate.error);
	EXPECT_EQ(ForwardError::DegenerateScale, bad_candidate.forward_error);

	auto invalid_target = input.target;
	std::swap(invalid_target[1], invalid_target[3]);
	auto const bad_target = MeasurePerspectiveResidual(
		candidate, invalid_target, input.output_mapping);
	EXPECT_EQ(ResidualError::InvalidTarget, bad_target.error);
	EXPECT_NE(GeometryError::None, bad_target.geometry_error);

	auto const bad_mapping = MeasurePerspectiveResidual(
		candidate, input.target, {0.0, 1.0});
	EXPECT_EQ(ResidualError::InvalidOutputMapping, bad_mapping.error);
	EXPECT_STRNE("unknown Perspective residual error",
		DescribeResidualError(bad_mapping.error));

	auto invalid_samples = candidate;
	invalid_samples.bounds.residual_samples = {
		{MaxAbsCoordinate + 1.0, 0.0}};
	auto const bad_samples = MeasurePerspectiveResidual(
		invalid_samples, input.target, input.output_mapping);
	EXPECT_EQ(ResidualError::InvalidCandidate, bad_samples.error);
	EXPECT_EQ(ForwardError::InvalidBounds, bad_samples.forward_error);
}

TEST(perspective_tag_solver, drawing_control_samples_extend_residual_domain) {
	auto candidate = BaseInput();
	candidate.bounds = {
		{0.0, 0.0, 10.0, 7.5}, BoundsKind::Drawing,
		Resolution {10.0, 10.0}};
	candidate.state.position = {400.0, 300.0};

	auto target_input = candidate;
	target_input.state.rotation_x = 45.0;
	auto const target = ForwardQuad(target_input);
	ASSERT_TRUE(target) << DescribeForwardError(target.error);

	auto const rectangle_only = MeasurePerspectiveResidual(
		candidate, target.quad, {1.0, 1.0});
	ASSERT_TRUE(rectangle_only) << DescribeResidualError(rectangle_only.error);

	candidate.bounds.residual_samples = {
		{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}};
	auto const with_controls = MeasurePerspectiveResidual(
		candidate, target.quad, {1.0, 1.0});
	ASSERT_TRUE(with_controls) << DescribeResidualError(with_controls.error);
	EXPECT_GT(with_controls.max_error, rectangle_only.max_error);
}

TEST(perspective_tag_solver, residual_scan_keeps_the_last_sample_and_tiny_output_scales) {
	auto source = BaseInput();
	source.bounds.residual_samples = {{.x = 500.0, .y = 80.0}};
	auto desired = source.state;
	desired.scale_x = 150.0;
	auto const target = TargetFrom(source, desired);
	auto const ordinary = MeasurePerspectiveResidual(source, target, {.scale_x = 1.0, .scale_y = 1.0});
	ASSERT_TRUE(ordinary);
	EXPECT_NEAR(250.0, ordinary.max_error, 1.0e-9);
	auto const tiny = MeasurePerspectiveResidual(source, target, {.scale_x = 1.0e-200, .scale_y = 1.0e-200});
	ASSERT_TRUE(tiny);
	EXPECT_NEAR(250.0, tiny.max_error / 1.0e-200, 1.0e-9);
}

TEST(perspective_tag_solver, a_dragged_corner_under_fax_frz_only_settles_for_the_nearest_affine) {
	auto source = BaseInput();
	auto affine_state = source.state;
	affine_state.position = {520.0, 400.0};
	affine_state.scale_x = 118.0;
	affine_state.scale_y = 86.0;
	affine_state.shear_x = 0.21;
	affine_state.rotation_z = -11.0;
	auto target = TargetFrom(source, affine_state);
	// One corner off the parallelogram. Twist of 4 script px puts the best
	// affine fit ~1 px away, which no amount of decimal places can recover.
	target[0].x += 4.0;

	// The solver settles for the nearest affine and reports the shortfall
	// rather than refusing the drag.
	SolverInput input{source, target};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const snapped = SolvePerspectiveTags(input);
	ASSERT_TRUE(snapped) << DescribeSolverError(snapped.error);
	ASSERT_TRUE(snapped.candidate);
	EXPECT_TRUE(snapped.candidate->Snapped());
	EXPECT_GT(snapped.candidate->snap_error, input.max_error);
	EXPECT_LE(snapped.candidate->snap_error, 2.0);
	// Rounding stays on its own budget even though the model cannot reach the
	// drawn quad; measuring it against the drawn quad would force full digits.
	EXPECT_LE(snapped.candidate->quantization_error, input.max_error);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		snapped.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	ExpectSerializedAtMost(snapped.candidate->serialized, 6);
}

TEST(perspective_tag_solver, an_exact_candidate_always_beats_a_snapped_one) {
	auto source = BaseInput();
	auto moved = source.state;
	moved.position = {source.state.position.x + 1.0, source.state.position.y};

	SolverInput input {source, TargetFrom(source, moved)};
	// Generous enough that leaving the line untouched also fits the tolerance.
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	// Without snapped ranking first, NoOp would win on tag economy and a real
	// drag would silently do nothing.
	EXPECT_NE(CandidateFamily::NoOp, result.candidate->family);
	EXPECT_FALSE(result.candidate->Snapped());
	EXPECT_EQ(0, result.candidate->score.snapped);
	EXPECT_DOUBLE_EQ(0.0, result.candidate->snap_error);
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, preserve_scale_refits_instead_of_demanding_the_exact_size) {
	auto source = BaseInput();
	source.state.scale_x = 112.0;
	source.state.scale_y = 88.0;
	auto desired = source.state;
	desired.position = {515.0, 395.0};
	desired.shear_x = 0.18;
	desired.rotation_z = -9.0;
	auto target = TargetFrom(source, desired);
	// Enough corner error that the free-scale affine fit no longer recovers the
	// source scale exactly, which is all the old equality pre-filter tested.
	target[2].y += 3.0;

	SolverInput input {source, target};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	// A scale lock used to collapse to translation; the shape must now move.
	EXPECT_NE(CandidateFamily::Translation, result.candidate->family);
	EXPECT_NE(CandidateFamily::CurrentRepresentation, result.candidate->family);
	EXPECT_GT(std::abs(result.candidate->state.shear_x), 1.0e-3);
	EXPECT_LE(result.candidate->snap_error, 4.0);
}

TEST(perspective_tag_solver, preserve_scale_with_fax_frz_only_is_not_translation_only) {
	auto source = BaseInput();
	source.state.scale_x = 105.0;
	source.state.scale_y = 95.0;
	auto desired = source.state;
	desired.position = {505.0, 385.0};
	desired.shear_x = 0.24;
	desired.rotation_z = 13.0;
	auto target = TargetFrom(source, desired);
	target[1].x += 2.5;

	SolverInput input {source, target};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_GT(std::abs(result.candidate->state.shear_x), 1.0e-3);
	EXPECT_GT(std::abs(result.candidate->state.rotation_z), 1.0e-3);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		result.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
}

TEST(perspective_tag_solver, fax_frz_only_keeps_a_no_op_on_a_line_that_already_has_frx) {
	auto source = BaseInput();
	source.state.origin = Vec2 {430.0, 280.0};
	source.state.rotation_x = 15.0;
	source.state.rotation_y = -8.0;

	SolverInput input {source, TargetFrom(source, source.state)};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	// Judging the result shape absolutely used to reject even an untouched
	// line here, which is exactly when a typesetter reaches for the switch.
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::NoOp, result.candidate->family);
	EXPECT_EQ(0, result.candidate->score.changed_tag_count);
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(15.0, result.candidate->state.rotation_x);
}

TEST(perspective_tag_solver, fax_frz_only_refits_inside_the_plane_the_line_declares) {
	auto source = BaseInput();
	source.state.origin = Vec2 {430.0, 280.0};
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	// In-plane change only: the declared plane stays, pos/scale/fax/frz move.
	auto desired = source.state;
	desired.position = {455.0, 325.0};
	desired.scale_x = 122.0;
	desired.scale_y = 91.0;
	desired.shear_x = 0.19;
	desired.rotation_z = 7.0;

	SolverInput input {source, TargetFrom(source, desired)};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	// No other family reaches a projective target under this policy: the
	// affine ones must flatten the plane, and the projective ones are not
	// generated. Flattening is what made the switch feel like a dead end.
	EXPECT_EQ(CandidateFamily::CurrentPlaneRefit, result.candidate->family);
	EXPECT_FALSE(result.candidate->Snapped());
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(source.state.origin->x, result.candidate->state.origin->x);
	EXPECT_DOUBLE_EQ(source.state.origin->y, result.candidate->state.origin->y);
	EXPECT_DOUBLE_EQ(18.0, result.candidate->state.rotation_x);
	EXPECT_DOUBLE_EQ(-12.0, result.candidate->state.rotation_y);
	EXPECT_NEAR(desired.rotation_z, result.candidate->state.rotation_z, 1.0e-3);
	EXPECT_NEAR(desired.shear_x, result.candidate->state.shear_x, 1.0e-3);
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

// The visual tool re-solves on every mouse-move to draw the reachable shape, so
// a single solve has to fit inside a frame with room to spare. This guards the
// interaction, not the algorithm: the bound is deliberately loose enough to
// survive a slow CI machine while still failing if a solve becomes pathological.
TEST(perspective_tag_solver, a_single_solve_is_cheap_enough_for_a_mouse_move) {
	auto const source = BaseInput();
	// The expensive case: a projective target under the unrestricted policy, so
	// every candidate family including the implicit-origin optimizers is tried.
	EvaluatedTransformState dragged = source.state;
	dragged.position = {520.0, 360.0};
	dragged.scale_x = 118.0;
	dragged.scale_y = 92.0;
	dragged.shear_x = 0.16;
	dragged.rotation_x = 21.0;
	dragged.rotation_y = -17.0;
	dragged.rotation_z = 13.0;
	SolverInput input{source, TargetFrom(source, dragged)};
	ASSERT_TRUE(SolvePerspectiveTags(input));

	constexpr int kIterations = 200;
	auto const start = std::chrono::steady_clock::now();
	for (int iteration = 0; iteration < kIterations; ++iteration) {
		// Nudge the target each time so no cache or early-out can make the
		// repeated solve cheaper than the first one.
		auto nudged = input;
		nudged.target[2].x += 0.01 * iteration;
		EXPECT_TRUE(SolvePerspectiveTags(nudged));
	}
	auto const elapsed = std::chrono::steady_clock::now() - start;
	double const per_solve_ms = std::chrono::duration<double, std::milli>(
		elapsed).count() / kIterations;
	EXPECT_LT(per_solve_ms, 8.0) << per_solve_ms << " ms per solve";
}

// Probe: how far does the nearest representable shape land from an arbitrary
// hand-drawn quad under each policy? Documents which drags are exactly
// representable and which are only ever approximated -- the approximated ones
// come back as settled candidates with the shortfall reported.
TEST(perspective_tag_solver, arbitrary_corner_drag_reachability_by_policy) {
	auto const source = BaseInput();
	EvaluatedTransformState tilted = source.state;
	tilted.position = {500.0, 340.0};
	tilted.rotation_x = 18.0;
	tilted.rotation_y = -12.0;
	tilted.rotation_z = 7.0;
	auto const base = TargetFrom(source, tilted);

	auto shortfall = [&](Quad const& target,
						 PerspectiveRepresentationPolicy policy) {
		SolverInput input{source, target};
		input.representation_policy = policy;
		auto const solved = SolvePerspectiveTags(input);
		EXPECT_TRUE(solved) << DescribeSolverError(solved.error);
		return solved.candidate && !solved.candidate->Snapped()
				   ? 0.0
				   : solved.candidate->snap_error;
	};

	// Exactly representable: built from a real transform state, untouched.
	EXPECT_DOUBLE_EQ(0.0, shortfall(
							  base, PerspectiveRepresentationPolicy::Automatic));

	// One corner pulled 6 px off that perspective quad -- the eyeballed edge.
	Quad dragged = base;
	dragged[2].x += 6.0;
	dragged[2].y -= 3.0;
	double const automatic = shortfall(
		dragged, PerspectiveRepresentationPolicy::Automatic);
	double const restricted = shortfall(
		dragged, PerspectiveRepresentationPolicy::FaxFrzOnly);
	// The unrestricted policy reaches an arbitrary quad exactly: the projective
	// families have enough freedom that a pulled corner needs no snapping at
	// all. So on a tilted line under the default policy the tool reproduces
	// precisely what was drawn -- it does not quietly straighten anything, and
	// the snap machinery never engages.
	EXPECT_DOUBLE_EQ(0.0, automatic);
	// The restricted subset cannot: forced onto the affine subspace the drag
	// becomes a parallelogram, and the discarded twist is the shortfall. The
	// nearest affine is returned with that shortfall reported, and 6 px of
	// drag costs more than 6 px of shortfall because the twist is spread over
	// all four corners.
	EXPECT_GT(restricted, 0.0);
	EXPECT_LE(restricted, 16.0);
}

// The point of an edge anchor. One edge is aligned to something visible in the
// frame; the opposite edge is eyeballed. Plain least squares averages the two,
// which moves the edge that was already correct by half the eyeballed error --
// exactly the information the user was most sure of. Anchoring must hold it.
TEST(perspective_tag_solver, edge_anchor_keeps_the_referenced_edge_exact) {
	auto const source = BaseInput();
	EvaluatedTransformState flat = source.state;
	flat.position = {500.0, 340.0};
	flat.rotation_z = 8.0;
	// A genuine parallelogram: the shape the user is actually trying to hit.
	auto const truth = TargetFrom(source, flat);

	// Top edge (p0->p1) left exactly on the reference; the bottom edge was
	// eyeballed, one corner 10 px out and the other 4 px.
	Quad drawn = truth;
	drawn[3].y += 10.0;
	drawn[2].y += 4.0;

	auto landed_quad = [&](PerspectiveEdgeAnchor anchor) {
		SolverInput input{source, drawn};
		// FaxFrzOnly on purpose: under the unrestricted policy the drawn quad is
		// exactly representable, so no averaging happens and the anchor is moot.
		// The contamination only exists where the affine fit actually wins.
		input.representation_policy =
			PerspectiveRepresentationPolicy::FaxFrzOnly;
		input.edge_anchor = anchor;
		auto const solved = SolvePerspectiveTags(input);
		EXPECT_TRUE(solved) << DescribeSolverError(solved.error);
		auto forward = source;
		forward.state = solved.candidate->state;
		auto const landed = ForwardQuad(forward);
		EXPECT_TRUE(landed) << DescribeForwardError(landed.error);
		return landed.quad;
	};
	auto offset_from_truth = [&](Quad const& quad, std::size_t index) {
		return std::hypot(
			quad[index].x - truth[index].x, quad[index].y - truth[index].y);
	};

	// Unanchored: the correct edge is dragged off by averaging.
	auto const averaged = landed_quad(PerspectiveEdgeAnchor::None);
	EXPECT_GT(offset_from_truth(averaged, 0), 0.5);
	EXPECT_GT(offset_from_truth(averaged, 1), 0.5);

	// Anchored to the top: that edge has to come back exactly, and the whole
	// shortfall moves onto the edge whose corners were never trusted.
	auto const anchored = landed_quad(PerspectiveEdgeAnchor::Top);
	EXPECT_NEAR(0.0, offset_from_truth(anchored, 0), 1.0e-6);
	EXPECT_NEAR(0.0, offset_from_truth(anchored, 1), 1.0e-6);
	EXPECT_LT(offset_from_truth(anchored, 0), offset_from_truth(averaged, 0));
	EXPECT_LT(offset_from_truth(anchored, 1), offset_from_truth(averaged, 1));
	// Still a parallelogram, so the eyeballed edge lands parallel to the anchor
	// at the average of where its two corners were drawn.
	Vec2 const top = anchored[1] - anchored[0];
	Vec2 const bottom = anchored[2] - anchored[3];
	EXPECT_NEAR(0.0, top.x * bottom.y - top.y * bottom.x, 1.0e-6);
}

// Anchoring the opposite edge must hold that one instead. Guards the index and
// sign bookkeeping, which is the easy thing to get subtly wrong.
TEST(perspective_tag_solver, edge_anchor_honors_each_of_the_four_edges) {
	auto const source = BaseInput();
	EvaluatedTransformState flat = source.state;
	flat.position = {500.0, 340.0};
	flat.rotation_z = 8.0;
	auto const truth = TargetFrom(source, flat);

	struct Case {
		PerspectiveEdgeAnchor anchor;
		std::size_t from;
		std::size_t to;
		char const* name;
	};
	constexpr std::array<Case, 4> cases {{
		{PerspectiveEdgeAnchor::Top, 0, 1, "Top"},
		{PerspectiveEdgeAnchor::Right, 1, 2, "Right"},
		{PerspectiveEdgeAnchor::Bottom, 3, 2, "Bottom"},
		{PerspectiveEdgeAnchor::Left, 0, 3, "Left"},
	}};
	for (auto const& current : cases) {
		// Perturb only the corners that are not on the anchored edge, so the
		// anchored edge is by construction the correct one. The two offsets must
		// differ: moving both free corners by the same vector just translates
		// that edge and keeps the quad a parallelogram, which stays exactly
		// representable and so has no twist for the anchor to protect against.
		Quad drawn = truth;
		double scale = 1.0;
		for (std::size_t index = 0; index < drawn.size(); ++index) {
			if (index == current.from || index == current.to)
				continue;
			drawn[index].x += 7.0 * scale;
			drawn[index].y -= 5.0 * scale;
			scale = 2.5;
		}

		SolverInput input{source, drawn};
		input.representation_policy =
			PerspectiveRepresentationPolicy::FaxFrzOnly;
		input.edge_anchor = current.anchor;
		auto const solved = SolvePerspectiveTags(input);
		ASSERT_TRUE(solved) << current.name << ": "
			<< DescribeSolverError(solved.error);
		auto forward = source;
		forward.state = solved.candidate->state;
		auto const landed = ForwardQuad(forward);
		ASSERT_TRUE(landed) << current.name << ": "
			<< DescribeForwardError(landed.error);
		// Bounded by the tag rounding budget, not by zero: the anchor is exact
		// in the fit, but the emitted decimals still move the result slightly.
		// At maximum_decimals 0 nothing quantizes inside max_error at all, which
		// is what confirms this residual is serialization and not the anchor.
		for (std::size_t index : {current.from, current.to}) {
			EXPECT_NEAR(truth[index].x, landed.quad[index].x, input.max_error)
				<< current.name << " corner " << index;
			EXPECT_NEAR(truth[index].y, landed.quad[index].y, input.max_error)
				<< current.name << " corner " << index;
		}

		// And the anchor has to be doing real work: without it the same drag
		// pulls this edge far outside the rounding budget.
		SolverInput unanchored = input;
		unanchored.edge_anchor = PerspectiveEdgeAnchor::None;
		auto const averaged = SolvePerspectiveTags(unanchored);
		ASSERT_TRUE(averaged) << current.name;
		auto averaged_forward = source;
		averaged_forward.state = averaged.candidate->state;
		auto const averaged_quad = ForwardQuad(averaged_forward);
		ASSERT_TRUE(averaged_quad) << current.name;
		double worst_averaged = 0.0;
		for (std::size_t index : {current.from, current.to}) {
			worst_averaged = std::max(worst_averaged, std::hypot(
				averaged_quad.quad[index].x - truth[index].x,
				averaged_quad.quad[index].y - truth[index].y));
		}
		EXPECT_GT(worst_averaged, input.max_error) << current.name;
	}
}

// libass shears text per glyph line -- each line's own baseline is the shear
// reference and its layout offset is never sheared -- so the forward model's
// global x' = x + fax*y over the base rectangle is only faithful for one line.
// The planner used to take that model at face value and answer
// predicted_error = 0 for a sheared multi-line target that libass renders
// with the second line unmoved. The solver now freezes both shear axes for
// multi-line bounds and reports the shortfall instead.
TEST(perspective_tag_solver, multiline_text_bounds_freeze_shear_instead_of_claiming_exact_reach) {
	auto source = BaseInput();
	source.bounds.multiline_text = true;
	auto sheared = source.state;
	sheared.shear_x = 1.0;
	auto const target = TargetFrom(source, sheared);

	SolverInput input{source, target};
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(0.0, result.candidate->state.shear_x);
	EXPECT_DOUBLE_EQ(0.0, result.candidate->state.shear_y);
	EXPECT_TRUE(result.candidate->Snapped());
	EXPECT_GT(result.candidate->snap_error, input.max_error);

	// The same sheared target from single-line bounds stays exactly
	// reachable through \fax, so the freeze above is what changed the answer.
	auto single = source;
	single.bounds.multiline_text = false;
	SolverInput single_input{single, target};
	auto const single_result = SolvePerspectiveTags(single_input);
	ASSERT_TRUE(single_result) << DescribeSolverError(single_result.error);
	ASSERT_TRUE(single_result.candidate);
	EXPECT_FALSE(single_result.candidate->Snapped());
	EXPECT_NEAR(1.0, single_result.candidate->state.shear_x, 1.0e-6);
	EXPECT_LE(single_result.candidate->max_error, single_input.max_error);
}

// Overwriting a fitted shear after the fact invalidated the geometry the fit
// had chosen for the remaining parameters: a target that is exactly the
// source state with only \frx changed -- both shears retained -- used to come
// back snapped with a 10 px shortfall. With the shear lock applied inside the
// fit, the tilt carries the whole drag and both shears survive bit for bit.
TEST(perspective_tag_solver, multiline_shear_lock_is_a_fitting_constraint_not_a_post_fit_restore) {
	auto source = BaseInput();
	source.bounds.multiline_text = true;
	source.state.shear_x = 0.2;
	source.state.shear_y = 0.1;
	auto tilted = source.state;
	tilted.rotation_x = 30.0;
	auto const target = TargetFrom(source, tilted);

	SolverInput input{source, target};
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(0.2, result.candidate->state.shear_x);
	EXPECT_DOUBLE_EQ(0.1, result.candidate->state.shear_y);
	EXPECT_NEAR(30.0, result.candidate->state.rotation_x, 1.0e-3);
	EXPECT_FALSE(result.candidate->Snapped());
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, multiline_free_rotations_match_serialized_geometry) {
	auto source = BaseInput();
	source.bounds.multiline_text = true;
	for (auto const tilt : {Vec2{.x = 30.4, .y = 0.0}, Vec2{.x = 0.0, .y = -20.4}}) {
		SCOPED_TRACE(FormatAssPoint(tilt, 1));
		auto desired = source.state;
		desired.rotation_x = tilt.x;
		desired.rotation_y = tilt.y;
		for (int const decimals : {0, 1, 4, 6}) {
			SCOPED_TRACE(decimals);
			SolverInput input{.source = source, .target = TargetFrom(source, desired)};
			input.maximum_decimals = decimals;
			auto const result = SolvePerspectiveTags(input);
			if (!result) {
				EXPECT_EQ(0, decimals);
				EXPECT_EQ(NoFeasibleReason::Quantization, result.no_feasible_reason);
				continue;
			}
			auto const& candidate = *result.candidate;
			auto serialized = candidate.state;
			serialized.rotation_x = std::stod(candidate.serialized.rotation_x);
			serialized.rotation_y = std::stod(candidate.serialized.rotation_y);
			auto const model_quad = TargetFrom(source, candidate.state);
			auto const serialized_quad = TargetFrom(source, serialized);
			for (std::size_t index = 0; index < model_quad.size(); ++index) {
				EXPECT_LE(std::hypot(
							  serialized_quad[index].x - model_quad[index].x,
							  serialized_quad[index].y - model_quad[index].y),
						  input.max_error);
			}
		}
	}
}

// The frozen layout shear must also survive the digits: a pure translation
// of a multi-line line keeps both source tags bit-exact at every decimal
// setting, instead of CompactField rounding \fax0.123456 down to 0.123 (or
// zero decimals rejecting a plan that only needed the integer position).
TEST(perspective_tag_solver, multiline_shear_survives_quantization_at_every_decimal_setting) {
	auto source = BaseInput();
	source.bounds.multiline_text = true;
	source.state.shear_x = 0.123456;
	source.state.shear_y = 0.234567;
	auto moved = source.state;
	moved.position = moved.position + Vec2 {20.0, 0.0};
	Quad const target = TargetFrom(source, moved);

	for (int const decimals : {0, 3, 6}) {
		SolverInput input{source, target};
		input.maximum_decimals = decimals;
		auto const result = SolvePerspectiveTags(input);
		ASSERT_TRUE(result) << decimals << ": "
			<< DescribeSolverError(result.error);
		ASSERT_TRUE(result.candidate);
		EXPECT_DOUBLE_EQ(0.123456, result.candidate->state.shear_x) << decimals;
		EXPECT_DOUBLE_EQ(0.234567, result.candidate->state.shear_y) << decimals;
		EXPECT_FALSE(result.candidate->Snapped()) << decimals;
		EXPECT_LE(result.candidate->max_error, input.max_error) << decimals;
	}
}

// The anchor is a promise about the drawn edge, so it has to outlive the fit
// that seeded it. A translation of this target scores a smaller max residual
// than the anchored affine fit (20 px spread over all corners versus 40 px on
// the eyeballed edge), and that translation moves the held top edge by 20 px.
// Scoring alone used to hand it the win; the anchor now disqualifies it.
TEST(perspective_tag_solver, edge_anchor_disqualifies_candidates_that_move_the_held_edge) {
	auto const source = BaseInput();
	Quad const target {{{250.0, 200.0}, {410.0, 200.0}, {450.0, 280.0}, {210.0, 280.0}}};

	SolverInput input{source, target};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	input.edge_anchor = PerspectiveEdgeAnchor::Top;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	auto forward = source;
	forward.state = result.candidate->state;
	auto const landed = ForwardQuad(forward);
	ASSERT_TRUE(landed) << DescribeForwardError(landed.error);
	EXPECT_NEAR(target[0].x, landed.quad[0].x, input.max_error);
	EXPECT_NEAR(target[0].y, landed.quad[0].y, input.max_error);
	EXPECT_NEAR(target[1].x, landed.quad[1].x, input.max_error);
	EXPECT_NEAR(target[1].y, landed.quad[1].y, input.max_error);

	// Without the anchor the same drag still answers with a shape whose top
	// edge sits far from the drawn one -- the behavior the anchor exists to
	// prevent, kept honest as the control.
	SolverInput unanchored = input;
	unanchored.edge_anchor = PerspectiveEdgeAnchor::None;
	auto const averaged = SolvePerspectiveTags(unanchored);
	ASSERT_TRUE(averaged) << DescribeSolverError(averaged.error);
	auto averaged_forward = source;
	averaged_forward.state = averaged.candidate->state;
	auto const averaged_landed = ForwardQuad(averaged_forward);
	ASSERT_TRUE(averaged_landed) << DescribeForwardError(averaged_landed.error);
	EXPECT_GT(std::hypot(
		averaged_landed.quad[0].x - target[0].x,
		averaged_landed.quad[0].y - target[0].y), input.max_error);
}

// The anchored edge is measured against the drawn quad itself. Under Preserve
// the affine families aim at the area-normalized quad, which silently moved
// the held edge to where the pinned scale could produce it; a scale lock and
// a drawn edge the pinned scale cannot produce are contradictory demands, and
// the solver now reports that conflict instead of satisfying the smaller one.
TEST(perspective_tag_solver, preserve_anchor_conflicts_with_an_edge_the_pinned_scale_cannot_produce) {
	auto const source = BaseInput();
	Quad const target {{{300.0, 220.0}, {700.0, 220.0},
		{700.0, 380.0}, {300.0, 380.0}}};

	for (auto const policy : {PerspectiveRepresentationPolicy::Automatic,
			PerspectiveRepresentationPolicy::FaxFrzOnly}) {
		SolverInput input{source, target};
		input.scale_policy = PerspectiveScalePolicy::Preserve;
		input.representation_policy = policy;
		input.edge_anchor = PerspectiveEdgeAnchor::Top;
		auto const result = SolvePerspectiveTags(input);
		EXPECT_FALSE(result) << static_cast<int>(policy);
		EXPECT_EQ(SolverError::NoFeasibleCandidate, result.error) << static_cast<int>(policy);
		EXPECT_EQ(NoFeasibleReason::EdgeAnchor, result.no_feasible_reason)
			<< static_cast<int>(policy);
	}
}

// A quantization-stage death must not outrank an anchor the candidate could
// never satisfy: with the rotation locked to zero no model tilts the held
// edge, so the conflict is the anchor's regardless of the digit budget.
TEST(perspective_tag_solver, quantization_failure_cannot_mask_an_unreachable_anchor) {
	auto const source = BaseInput();
	Quad const target {{{300.25, 220.25}, {500.25, 240.25},
		{480.25, 320.25}, {280.25, 300.25}}};

	SolverInput input{source, target};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	input.edge_anchor = PerspectiveEdgeAnchor::Top;
	input.locked_rotation_z = 0.0;
	input.maximum_decimals = 0;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_FALSE(result);
	EXPECT_EQ(SolverError::NoFeasibleCandidate, result.error);
	EXPECT_EQ(NoFeasibleReason::EdgeAnchor, result.no_feasible_reason);
}

// Probe: how far does the nearest representable shape land for the kind of
// drag a typesetter makes? Preserve + FaxFrzOnly is the most constrained
// pairing and the one users report as least faithful.
// The tolerance a drag needs is the shape error it carries, never the size
// error. Hand-placed corners always drift a few percent in size; under a scale
// lock that drift is not something the user was asked to avoid, so charging it
// to the acceptance budget refuses drags for an unactionable reason. A 4% drift
// on this 200x80 box is 8 output pixels, twice the shipped default.
TEST(perspective_tag_solver, a_scale_lock_does_not_charge_for_size_drift) {
	auto const source = BaseInput();
	EvaluatedTransformState tilted = source.state;
	tilted.position = {500.0, 340.0};
	tilted.rotation_z = 8.0;
	tilted.shear_x = 0.12;
	auto const base = TargetFrom(source, tilted);

	// The solve always returns its best candidate now, so "what it costs" is
	// the shortfall the winner reports, not a tolerance that had to be raised.
	auto shortfall = [&](Quad const& target, PerspectiveScalePolicy scale,
						 PerspectiveRepresentationPolicy representation) {
		SolverInput input{source, target};
		input.scale_policy = scale;
		input.representation_policy = representation;
		auto const solved = SolvePerspectiveTags(input);
		EXPECT_TRUE(solved) << DescribeSolverError(solved.error);
		return solved.candidate && !solved.candidate->Snapped()
				   ? 0.0
				   : solved.candidate->snap_error;
	};

	// Uniform 4% enlargement about the centre: zero twist, so the only thing
	// wrong with it is the size.
	Quad resized = base;
	for (auto& point : resized) {
		point.x = 500.0 + (point.x - 500.0) * 1.04;
		point.y = 340.0 + (point.y - 340.0) * 1.04;
	}
	// One corner pulled off the parallelogram: real shape error, and it must
	// still cost what it costs.
	Quad twisted = base;
	twisted[2].x += 5.0; twisted[2].y += 2.0;
	Quad realistic = resized;
	realistic[2].x += 5.0; realistic[2].y += 2.0;

	constexpr auto kPreserve = PerspectiveScalePolicy::Preserve;
	constexpr auto kFit = PerspectiveScalePolicy::Fit;
	constexpr auto kFaxFrz = PerspectiveRepresentationPolicy::FaxFrzOnly;
	constexpr auto kAuto = PerspectiveRepresentationPolicy::Automatic;

	// Pure size drift is free under the locked scale, because the drawn size is
	// the one thing the policy says to ignore.
	EXPECT_DOUBLE_EQ(0.0, shortfall(resized, kPreserve, kFaxFrz));
	// Unpinned scale reaches it outright, as it always did.
	EXPECT_DOUBLE_EQ(0.0, shortfall(resized, kFit, kFaxFrz));

	// A twist still costs its own shortfall, and adding size drift on top does
	// not raise the bill (the size-blind fit may even land a hair closer).
	double const twist_only = shortfall(twisted, kPreserve, kFaxFrz);
	EXPECT_GT(twist_only, 0.0);
	EXPECT_LE(shortfall(realistic, kPreserve, kFaxFrz), twist_only + 1.0e-6);

	// The representation switch only decides whether the projective families
	// run; the area-pinned families are size-blind under the lock either way,
	// so pure size drift is equally free under Automatic.
	EXPECT_DOUBLE_EQ(0.0, shortfall(resized, kPreserve, kAuto));
}

// Preserve keeps the drawn projective shape and chooses its uniform size from
// the pinned source scale. Quantization may move each corner by the normal
// output budget, but it must not introduce an independent corner distortion.
TEST(perspective_tag_solver, preserve_scale_keeps_a_foreshortened_shape_similar) {
	auto source = BaseInput();
	EvaluatedTransformState projective = source.state;
	projective.position = {560.0, 380.0};
	projective.scale_x = 190.0;
	projective.scale_y = 190.0;
	projective.rotation_x = 16.0;
	projective.rotation_y = -11.0;
	projective.rotation_z = 7.0;
	Quad const drawn = TargetFrom(source, projective);

	SolverInput input{source, drawn};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_NE(CandidateFamily::NoOp, result.candidate->family);
	EXPECT_NE(CandidateFamily::Translation, result.candidate->family);
	EXPECT_NE(CandidateFamily::CurrentRepresentation, result.candidate->family);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_LE(result.candidate->snap_error, input.max_error);

	auto landed = source;
	landed.state = result.candidate->state;
	auto const landed_quad = ForwardQuad(landed);
	ASSERT_TRUE(landed_quad) << DescribeForwardError(landed_quad.error);
	Vec2 mean;
	for (auto const& point : drawn)
		mean = mean + point / 4.0;
	double numerator = 0.0;
	double denominator = 0.0;
	for (std::size_t index = 0; index < drawn.size(); ++index) {
		Vec2 const source_vector = drawn[index] - mean;
		Vec2 const landed_vector = landed_quad.quad[index] - mean;
		numerator += landed_vector.Dot(source_vector);
		denominator += source_vector.SquareLength();
	}
	ASSERT_GT(denominator, 0.0);
	double const factor = numerator / denominator;
	ASSERT_GT(factor, 0.0);
	for (std::size_t index = 0; index < drawn.size(); ++index) {
		Vec2 const expected = mean + (drawn[index] - mean) * factor;
		EXPECT_NEAR(expected.x, landed_quad.quad[index].x, 2.0 * input.max_error)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, landed_quad.quad[index].y, 2.0 * input.max_error)
			<< "corner " << index;
	}

	// The published aim is the same homothetic target used by the solver, so
	// downstream staged verification sees the shape rather than the drawn size.
	numerator = denominator = 0.0;
	for (std::size_t index = 0; index < drawn.size(); ++index) {
		Vec2 const source_vector = drawn[index] - mean;
		Vec2 const aimed_vector = result.effective_target[index] - mean;
		numerator += aimed_vector.Dot(source_vector);
		denominator += source_vector.SquareLength();
	}
	double const aimed_factor = numerator / denominator;
	for (std::size_t index = 0; index < drawn.size(); ++index) {
		Vec2 const expected = mean + (drawn[index] - mean) * aimed_factor;
		EXPECT_NEAR(expected.x, result.effective_target[index].x, 1.0e-6)
			<< "aimed corner " << index;
		EXPECT_NEAR(expected.y, result.effective_target[index].y, 1.0e-6)
			<< "aimed corner " << index;
	}
}

// The solver must publish the quad it aimed at, or downstream verification
// re-charges the size error this policy just dropped.
TEST(perspective_tag_solver, effective_target_reports_the_quad_actually_aimed_at) {
	auto const source = BaseInput();
	EvaluatedTransformState tilted = source.state;
	tilted.position = {500.0, 340.0};
	tilted.rotation_z = 8.0;
	tilted.shear_x = 0.12;
	auto const base = TargetFrom(source, tilted);
	Quad resized = base;
	for (auto& point : resized) {
		point.x = 500.0 + (point.x - 500.0) * 1.04;
		point.y = 340.0 + (point.y - 340.0) * 1.04;
	}

	SolverInput input {source, resized};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const solved = SolvePerspectiveTags(input);
	ASSERT_TRUE(solved) << DescribeSolverError(solved.error);

	// Shrunk 4% about the drawn quad's own corner mean: the size is dropped,
	// but where the user put the box is not. Note this is not the same as
	// getting `base` back -- the drag was built by scaling about the \pos
	// corner, so `base` and the drawn quad have different centres, and honouring
	// the drawn centre is the whole point.
	Vec2 mean;
	for (auto const& point : resized)
		mean = mean + point / 4.0;
	for (std::size_t index = 0; index < resized.size(); ++index) {
		Vec2 const expected = mean + (resized[index] - mean) / 1.04;
		EXPECT_NEAR(expected.x, solved.effective_target[index].x, 1.0e-6)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, solved.effective_target[index].y, 1.0e-6)
			<< "corner " << index;
	}
	// Which lands it on exactly the area the pinned scale can produce.
	EXPECT_NEAR(std::abs(SignedArea(base)),
		std::abs(SignedArea(solved.effective_target)), 1.0e-6);

	// Untouched when no normalization applies.
	SolverInput fitting = input;
	fitting.scale_policy = PerspectiveScalePolicy::Fit;
	auto const fitted = SolvePerspectiveTags(fitting);
	ASSERT_TRUE(fitted) << DescribeSolverError(fitted.error);
	for (std::size_t index = 0; index < resized.size(); ++index) {
		EXPECT_NEAR(resized[index].x, fitted.effective_target[index].x, 1.0e-9);
		EXPECT_NEAR(resized[index].y, fitted.effective_target[index].y, 1.0e-9);
	}
}

// The point of the fay-preserving affine refit: under a scale lock a line that
// already declares \fay must be able to move in plane without the solver
// flattening the tag. The fax closed forms could only fit this line by zeroing
// the \fay -- policy-legal for an org-less line, but it gives up a degree of
// freedom and rewrites a tag the user wrote. The uniform scale in the target is
// size drift, which Preserve drops by normalizing the affine target back to the
// current area; what is left is exactly the source shape rotated and moved, so
// the refit lands on it exactly with the fay bit-identical.
TEST(perspective_tag_solver, fax_frz_only_refits_a_fay_line_and_keeps_the_fay_bit_exact) {
	auto source = BaseInput();
	source.state.shear_y = 0.1;
	auto desired = source.state;
	desired.position = {465.0, 345.0};
	desired.scale_x = 105.0;
	desired.scale_y = 105.0;
	desired.rotation_z = 8.0;

	SolverInput input {source, TargetFrom(source, desired)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::AffineFay, result.candidate->family);
	// The frozen slot reproduces the source tag bit for bit; the free in-plane
	// slots actually moved, so this is a refit and not a disguised no-op.
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_NEAR(desired.rotation_z, result.candidate->state.rotation_z, 1.0e-3);
	EXPECT_GT(std::hypot(
		result.candidate->state.position.x - source.state.position.x,
		result.candidate->state.position.y - source.state.position.y), 20.0);
	EXPECT_FALSE(result.candidate->Snapped());
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		source.state, result.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

// Same drag, but the line already carries \fax as well as \fay. The
// fay-preserving refit freezes only the fay axis, so the fax axis stays alive
// at the value the line already had -- neither shear is zeroed or rewritten.
TEST(perspective_tag_solver, fax_frz_only_refits_a_faxed_and_fayed_line_keeping_both_shears) {
	auto source = BaseInput();
	source.state.shear_x = 0.15;
	source.state.shear_y = 0.1;
	auto desired = source.state;
	desired.position = {465.0, 345.0};
	desired.scale_x = 105.0;
	desired.scale_y = 105.0;
	desired.rotation_z = -6.0;

	SolverInput input {source, TargetFrom(source, desired)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::AffineFay, result.candidate->family);
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	// The fax axis is free, and since the target keeps the source's fax shape
	// the fit holds it there -- preserved, not flattened to make room.
	EXPECT_NEAR(source.state.shear_x, result.candidate->state.shear_x, 1.0e-6);
	EXPECT_NEAR(desired.rotation_z, result.candidate->state.rotation_z, 1.0e-3);
	EXPECT_GT(std::hypot(
		result.candidate->state.position.x - source.state.position.x,
		result.candidate->state.position.y - source.state.position.y), 20.0);
	EXPECT_FALSE(result.candidate->Snapped());
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

// The plane refit for a line that declares org + frx/fry AND a \fay: the fay is
// one of the frozen restricted tags, so the in-plane solve must keep all of
// them bit-identical while still moving frz/fax/pos/scale. The old hard-zeroed
// parameterization could not express this candidate at all.
TEST(perspective_tag_solver, fax_frz_only_preserves_the_fay_inside_the_declared_plane) {
	auto source = BaseInput();
	source.state.origin = Vec2 {430.0, 280.0};
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	source.state.shear_y = 0.1;
	// In-plane change only: the declared plane and the fay stay, the rest moves.
	auto desired = source.state;
	desired.position = {455.0, 330.0};
	desired.scale_x = 122.0;
	desired.scale_y = 91.0;
	desired.shear_x = 0.19;
	desired.rotation_z = 7.0;

	SolverInput input {source, TargetFrom(source, desired)};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::CurrentPlaneRefit, result.candidate->family);
	// Every restricted tag numerically identical to the source.
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(source.state.origin->x, result.candidate->state.origin->x);
	EXPECT_DOUBLE_EQ(source.state.origin->y, result.candidate->state.origin->y);
	EXPECT_DOUBLE_EQ(source.state.rotation_x, result.candidate->state.rotation_x);
	EXPECT_DOUBLE_EQ(source.state.rotation_y, result.candidate->state.rotation_y);
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	// ... while the in-plane tags carry the whole drag.
	EXPECT_NEAR(desired.shear_x, result.candidate->state.shear_x, 1.0e-3);
	EXPECT_NEAR(desired.rotation_z, result.candidate->state.rotation_z, 1.0e-3);
	EXPECT_NEAR(desired.scale_x, result.candidate->state.scale_x, 1.0e-3);
	EXPECT_NEAR(desired.scale_y, result.candidate->state.scale_y, 1.0e-3);
	EXPECT_NEAR(desired.position.x, result.candidate->state.position.x, 1.0e-3);
	EXPECT_NEAR(desired.position.y, result.candidate->state.position.y, 1.0e-3);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		source.state, result.candidate->state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

// Vertical CJK faces: the rotation lock used to skip the plane refit entirely,
// because freeing frz could only fail the lock filter. With the refit freezing
// frz at the lock the candidate is legal by construction -- and the frozen slot
// must serialize as the line's own 270 degrees, not the remainder-wrapped -90
// the optimizer's angle wrapping would produce if it ever touched a locked
// slot. The \fay the line carries survives as well.
TEST(perspective_tag_solver, locked_rotation_plane_refit_serializes_the_frozen_rotation) {
	auto source = BaseInput();
	source.state.origin = Vec2 {430.0, 280.0};
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	source.state.shear_y = 0.08;
	source.state.rotation_z = 270.0;
	auto desired = source.state;
	desired.position = {470.0, 340.0};
	desired.scale_x = 115.0;
	desired.scale_y = 95.0;
	desired.shear_x = 0.2;

	SolverInput input {source, TargetFrom(source, desired)};
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	input.locked_rotation_z = 270.0;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::CurrentPlaneRefit, result.candidate->family);
	EXPECT_EQ("270", result.candidate->serialized.rotation_z);
	EXPECT_DOUBLE_EQ(270.0, result.candidate->state.rotation_z);
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	ASSERT_TRUE(result.candidate->state.origin);
	EXPECT_DOUBLE_EQ(source.state.origin->x, result.candidate->state.origin->x);
	EXPECT_DOUBLE_EQ(source.state.rotation_x, result.candidate->state.rotation_x);
	EXPECT_DOUBLE_EQ(source.state.rotation_y, result.candidate->state.rotation_y);
	EXPECT_NEAR(desired.shear_x, result.candidate->state.shear_x, 1.0e-3);
	EXPECT_NEAR(desired.scale_x, result.candidate->state.scale_x, 1.0e-3);
	EXPECT_NEAR(desired.position.x, result.candidate->state.position.x, 1.0e-3);
	EXPECT_FALSE(result.candidate->Snapped());
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

// Per-candidate effective targets, projective side: moving a plane-bearing line
// inside its own declared plane changes the apparent area (the shifted position
// slides the depth profile), so under a scale lock the old global rescale used
// to shrink the drawn quad to the source's area and aim the plane refit at a
// quad the user never drew -- refusing sizes the declared plane realizes
// verbatim. The refit must aim at the drawn quad itself and hit it exactly.
// An in-plane move of a tilted org-bearing line under the restricted policy.
// The drawn quad is a pure translation of the current one, so the area
// normalization is the identity here and the plane refit must land the move
// while keeping org, frx, fry and the pinned scale exactly.
TEST(perspective_tag_solver, plane_refit_tracks_an_inplane_move_under_a_scale_lock) {
	auto source = BaseInput();
	source.state.origin = Vec2 {430.0, 280.0};
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	// Same scale, rotation and shear as the source: an in-plane move whose only
	// apparent-area change comes from perspective foreshortening.
	auto desired = source.state;
	desired.position = {470.0, 340.0};
	Quad const target = TargetFrom(source, desired);

	SolverInput input {source, target};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_EQ(CandidateFamily::CurrentPlaneRefit, result.candidate->family);
	// Same area in, same area out: the normalization degenerates to the drawn
	// quad up to floating-point noise.
	for (std::size_t index = 0; index < target.size(); ++index) {
		EXPECT_NEAR(target[index].x, result.effective_target[index].x, 1.0e-6)
			<< "corner " << index;
		EXPECT_NEAR(target[index].y, result.effective_target[index].y, 1.0e-6)
			<< "corner " << index;
	}
	// The scale lock is still honoured, and the drag really happened.
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_NEAR(desired.position.x, result.candidate->state.position.x, 1.0e-3);
	EXPECT_NEAR(desired.position.y, result.candidate->state.position.y, 1.0e-3);
	EXPECT_FALSE(result.candidate->Snapped());
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, preserve_scale_plane_targets_are_homothetic) {
	auto source = BaseInput();
	source.state.origin = Vec2{430.0, 280.0};
	source.state.rotation_x = 18.0;
	source.state.rotation_y = -12.0;
	auto desired = source.state;
	desired.position = {470.0, 340.0};
	Quad const target = TargetFrom(source, desired);
	SolverInput input{source, target};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::Automatic;

	auto const current = ForwardQuad(source);
	ASSERT_TRUE(current) << DescribeForwardError(current.error);
	auto const target_area = std::abs(SignedArea(target));
	auto const current_area = std::abs(SignedArea(current.quad));
	ASSERT_GT(target_area, 0.0);
	ASSERT_GT(current_area, 0.0);
	double const factor = std::sqrt(current_area / target_area);
	Vec2 center;
	for (auto const& point : target)
		center = center + point / 4.0;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	for (std::size_t index = 0; index < target.size(); ++index) {
		Vec2 const expected = center + (target[index] - center) * factor;
		EXPECT_NEAR(expected.x, result.effective_target[index].x, 1.0e-6)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, result.effective_target[index].y, 1.0e-6)
			<< "corner " << index;
	}
}

// Per-candidate effective targets, affine side: a fay-only line has no
// projective family under the restricted policy, so its candidates are affine
// and must KEEP the area normalization -- without it the pinned-scale refit
// would be charged the drawn 4% size drift as model error and the shortfall
// would balloon by the drift.
TEST(perspective_tag_solver, fay_lines_keep_the_area_normalized_target_for_affine_families) {
	auto source = BaseInput();
	source.state.shear_y = 0.1;
	auto const current = TargetFrom(source, source.state);

	// Uniform 4% enlargement about a point that is not the quad's own corner
	// mean, so the normalized quad is not merely a translation of the current
	// one and the NoOp cannot trivially win.
	Quad drawn;
	Vec2 const anchor {430.0, 310.0};
	for (std::size_t index = 0; index < current.size(); ++index)
		drawn[index] = anchor + (current[index] - anchor) * 1.04;

	SolverInput input {source, drawn};
	input.scale_policy = PerspectiveScalePolicy::Preserve;
	input.representation_policy = PerspectiveRepresentationPolicy::FaxFrzOnly;
	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	// A uniform enlargement normalizes back to a pure translation of the
	// current quad, so the tag-preserving translation candidate and the
	// fay-preserving refit land on the same state; the winner is then decided
	// by family rank. Either way the fay survives bit-exact.
	EXPECT_TRUE(result.candidate->family == CandidateFamily::CurrentRepresentation
		|| result.candidate->family == CandidateFamily::AffineFay)
		<< static_cast<int>(result.candidate->family);
	EXPECT_DOUBLE_EQ(source.state.shear_y, result.candidate->state.shear_y);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_FALSE(result.candidate->Snapped());

	// The winner aimed at the drawn quad shrunk back to the current area about
	// the drawn quad's own corner mean.
	Vec2 mean;
	for (auto const& point : drawn)
		mean = mean + point / 4.0;
	for (std::size_t index = 0; index < drawn.size(); ++index) {
		Vec2 const expected = mean + (drawn[index] - mean) / 1.04;
		EXPECT_NEAR(expected.x, result.effective_target[index].x, 1.0e-6)
			<< "corner " << index;
		EXPECT_NEAR(expected.y, result.effective_target[index].y, 1.0e-6)
			<< "corner " << index;
	}
	EXPECT_NEAR(std::abs(SignedArea(current)),
		std::abs(SignedArea(result.effective_target)), 1.0e-6);
}
