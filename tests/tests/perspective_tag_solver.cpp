#include <main.h>

#include "../../src/perspective_tag_solver.h"

#include <array>
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

TEST(perspective_tag_solver, fax_frz_only_rejects_forbidden_current_representation) {
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
	EXPECT_EQ(SolverError::NoFeasibleCandidate, result.error);
	EXPECT_FALSE(result.candidate);
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

TEST(perspective_tag_solver, preserve_scale_rejects_target_that_requires_scaling) {
	auto source = BaseInput();
	auto target = source.state;
	target.scale_x = 175.0;
	target.scale_y = 160.0;
	SolverInput input {source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto const result = SolvePerspectiveTags(input);
	EXPECT_EQ(SolverError::NoFeasibleCandidate, result.error);
	EXPECT_FALSE(result.candidate);
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

TEST(perspective_tag_solver, preserve_scale_keeps_non_integer_style_scale_exact) {
	auto source = BaseInput();
	source.state.scale_x = 123.456789;
	source.state.scale_y = 87.654321;
	auto target = source.state;
	target.position = {535.0, 405.0};
	SolverInput input {source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_EQ(1, result.candidate->score.changed_tag_count);
	EXPECT_LE(result.candidate->max_error, input.max_error);
}

TEST(perspective_tag_solver, preserve_scale_freezes_implicit_optimizer_scale_parameters) {
	auto source = BaseInput();
	source.state.scale_x = 121.234567;
	source.state.scale_y = 88.765432;
	auto target = source.state;
	target.position = {560.0, 390.0};
	target.shear_x = 0.16;
	target.rotation_x = 21.0;
	target.rotation_y = -17.0;
	target.rotation_z = 13.0;
	SolverInput input {source, TargetFrom(source, target)};
	input.scale_policy = PerspectiveScalePolicy::Preserve;

	auto const result = SolvePerspectiveTags(input);
	ASSERT_TRUE(result) << DescribeSolverError(result.error);
	ASSERT_TRUE(result.candidate);
	EXPECT_TRUE(
		result.candidate->family == CandidateFamily::ProjectiveImplicitFax
		|| result.candidate->family == CandidateFamily::ProjectiveImplicitFay);
	EXPECT_DOUBLE_EQ(source.state.scale_x, result.candidate->state.scale_x);
	EXPECT_DOUBLE_EQ(source.state.scale_y, result.candidate->state.scale_y);
	EXPECT_LE(result.candidate->max_error, input.max_error);
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
	EXPECT_EQ(SolverError::NoFeasibleCandidate, constrained.error);
	EXPECT_FALSE(constrained.candidate);
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
	EXPECT_EQ(SolverError::NoFeasibleCandidate, constrained_projective.error);
	EXPECT_FALSE(constrained_projective.candidate);
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
