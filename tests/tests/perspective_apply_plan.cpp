#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_style.h"
#include "../../src/perspective_apply_plan.h"
#include "../../src/perspective_quad_edit_state.h"
#include "../../src/subtitle_command_session.h"

#include <libaegisub/signal.h>

#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace perspective;

std::string DefaultStyleLine() {
	return "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,"
		"&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1";
}

std::string DrawingText(double x = 100.0, double y = 100.0) {
	return "{\\an7\\pos(" + FormatAssNumber(x, 4) + ","
		+ FormatAssNumber(y, 4)
		+ ")\\p1}m 0 0 l 100 0 100 50 0 50";
}

Quad Translate(Quad quad, double x, double y) {
	for (auto& point : quad) {
		point.x += x;
		point.y += y;
	}
	return quad;
}

bool FixedTextExtents(
	AssStyle*, std::string const& text,
	double& width, double& height, double& descent, double& external_leading) {
	width = static_cast<double>(text.size()) * 10.0;
	height = 20.0;
	descent = 4.0;
	external_leading = 2.0;
	return true;
}

Quad CurrentQuad(PerspectiveCaptureResult const& capture) {
	if (!capture.source || !capture.source->current_quad) {
		ADD_FAILURE() << "expected a projectable current subtitle quad";
		return {};
	}
	return *capture.source->current_quad;
}

struct ApplyFixture {
	AssFile file;
	AssDialogue* line = nullptr;
	PerspectiveApplyContext context;

	ApplyFixture() {
		file.Info.emplace_back("PlayResX", "640");
		file.Info.emplace_back("PlayResY", "480");
		file.Info.emplace_back("LayoutResX", "1280");
		file.Info.emplace_back("LayoutResY", "720");
		file.Info.emplace_back("WrapStyle", "0");
		file.Styles.push_back(*new AssStyle(DefaultStyleLine()));
		line = new AssDialogue;
		line->Row = 0;
		line->Start = 0;
		line->End = 5000;
		line->Text = DrawingText();
		file.Events.push_back(*line);

		context.frame_number = 12;
		context.capture_time_ms = 1000;
		context.play_resolution = {640.0, 480.0};
		context.layout_resolution = Resolution {1280.0, 720.0};
		context.video_storage_resolution = Resolution {1280.0, 720.0};
		context.output_mapping = {2.0, 1.5};
	}

	PerspectiveCaptureResult Capture() const {
		return CapturePerspectiveSource(file, *line, context);
	}
};

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

void ExpectLineMetadataUnchanged(
	AssDialogueBase const& before,
	AssDialogue const& after) {
	EXPECT_EQ(before.Id, after.Id);
	EXPECT_EQ(before.Row, after.Row);
	EXPECT_EQ(before.Comment, after.Comment);
	EXPECT_EQ(before.Layer, after.Layer);
	EXPECT_EQ(before.Margin, after.Margin);
	EXPECT_EQ(before.Start, after.Start);
	EXPECT_EQ(before.End, after.End);
	EXPECT_EQ(before.Style.get(), after.Style.get());
	EXPECT_EQ(before.Actor.get(), after.Actor.get());
	EXPECT_EQ(before.Effect.get(), after.Effect.get());
	EXPECT_EQ(before.ExtradataIds.get(), after.ExtradataIds.get());
}

void ExpectStaleAfterLineMutation(
	std::function<void(AssDialogue&)> const& mutate) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture);
	mutate(*fixture.line);
	EXPECT_FALSE(MatchesPerspectiveSource(
		fixture.file, fixture.context, capture.source->fingerprint));
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 1.0, 0.0));
	EXPECT_EQ(PerspectivePlanError::StaleSource, planned.error);
	EXPECT_FALSE(planned.plan);
}

void ExpectStaleAfterFixtureMutation(
	std::function<void(ApplyFixture&)> const& mutate) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture);
	auto const target = Translate(CurrentQuad(capture), 1.0, 0.0);
	auto const original_text = fixture.line->Text.get();
	mutate(fixture);
	EXPECT_FALSE(MatchesPerspectiveSource(
		fixture.file, fixture.context, capture.source->fingerprint));
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target);
	EXPECT_EQ(PerspectivePlanError::StaleSource, planned.error);
	EXPECT_FALSE(planned.plan);
	EXPECT_EQ(original_text, fixture.line->Text.get());
}
}

TEST(perspective_apply_plan, builds_immutable_rewrite_without_mutation_then_changes_only_text) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	auto const target = Translate(CurrentQuad(capture), 23.0, -11.0);
	auto const original_text = fixture.line->Text.get();
	AssDialogueBase const original = *fixture.line;
	auto* const original_pointer = fixture.line;

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_EQ(original_text, fixture.line->Text.get());
	EXPECT_NE(original_text, planned.plan->ReplacementText());
	EXPECT_LE(planned.plan->MaxError(), 0.1);
	EXPECT_EQ(fixture.line->Id, planned.plan->LineId());

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	EXPECT_EQ(original_pointer, executed.line);
	EXPECT_EQ(planned.plan->ReplacementText(), fixture.line->Text.get());
	ExpectLineMetadataUnchanged(original, *fixture.line);
}

TEST(perspective_apply_plan, ordinary_text_capture_and_apply_round_trip) {
	ApplyFixture fixture;
	fixture.line->Text = "ordinary text";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source->current_quad);
	EXPECT_EQ(BoundsKind::Text, capture.source->forward_input.bounds.kind);
	Quad const target {{{240.0, 160.0}, {400.0, 160.0},
		{400.0, 224.0}, {240.0, 224.0}}};
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.Bind(capture.source->current_quad, true));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.ReplaceTarget(target));

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);

	auto const recaptured = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	ASSERT_TRUE(recaptured.source->current_quad);
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.Bind(recaptured.source->current_quad, true, true));
	ASSERT_TRUE(state.Target());
	ASSERT_TRUE(state.Current());
	for (std::size_t index = 0; index < state.Target()->size(); ++index) {
		EXPECT_DOUBLE_EQ((*state.Current())[index].x, (*state.Target())[index].x);
		EXPECT_DOUBLE_EQ((*state.Current())[index].y, (*state.Target())[index].y);
	}
	EXPECT_FALSE(state.IsModified());
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target, fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
	EXPECT_NE(std::string::npos, fixture.line->Text.get().find("ordinary text"));
	EXPECT_EQ(std::string::npos, fixture.line->Text.get().find("\\p1"));
}

TEST(perspective_apply_plan, apply_elides_geometry_inherited_from_event_style) {
	ApplyFixture fixture;
	auto& style = fixture.file.Styles.front();
	style.scalex = 120.0;
	style.UpdateData();
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\fscx100\\fax0\\frx0\\p1}"
		"m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source->current_quad);
	EXPECT_DOUBLE_EQ(
		120.0, capture.source->state.event_style_transform.scale_x);
	EXPECT_DOUBLE_EQ(100.0, capture.source->state.transform.scale_x);

	auto target = *capture.source->current_quad;
	auto const top = target[1] - target[0];
	auto const bottom = target[2] - target[3];
	target[1] = target[0] + top * 1.2;
	target[2] = target[3] + bottom * 1.2;
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NEAR(120.0, planned.plan->Candidate().state.scale_x, 1.0e-9);
	auto const& replacement = planned.plan->ReplacementText();
	EXPECT_EQ(std::string::npos, replacement.find("\\fscx"));
	EXPECT_EQ(std::string::npos, replacement.find("\\fax"));
	EXPECT_EQ(std::string::npos, replacement.find("\\frx"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target, fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
}

TEST(perspective_apply_plan, scale_policy_preserve_ignores_size_and_fit_resizes) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	auto scaled_input = capture.source->forward_input;
	scaled_input.state.scale_x *= 1.5;
	scaled_input.state.scale_y *= 1.5;
	auto const scaled = ForwardQuad(scaled_input);
	ASSERT_TRUE(scaled) << DescribeForwardError(scaled.error);

	// Preserve says "do not change my font size", not "refuse a differently
	// sized quad": the area-pinned families aim at the drawn quad rescaled to
	// the area the source scale produces, so a uniformly larger target solves
	// at the original size.
	auto const preserve = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, scaled.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Preserve);
	ASSERT_TRUE(preserve) << DescribePerspectivePlanError(preserve.error);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_x,
					 preserve.plan->Candidate().state.scale_x);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_y,
					 preserve.plan->Candidate().state.scale_y);

	// Fit remains the only way to actually resize the text.
	auto const fit = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, scaled.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Fit);
	ASSERT_TRUE(fit) << DescribePerspectivePlanError(fit.error);
	EXPECT_NE(capture.source->state.transform.scale_x,
		fit.plan->Candidate().state.scale_x);
	EXPECT_NE(capture.source->state.transform.scale_y,
		fit.plan->Candidate().state.scale_y);

	auto const constrained_preserve = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, scaled.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Preserve,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	// The restricted representation runs the same size-blind normalization;
	// its narrower tag set still matches the shape at the source scale.
	ASSERT_TRUE(constrained_preserve)
		<< DescribePerspectivePlanError(constrained_preserve.error);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_x,
					 constrained_preserve.plan->Candidate().state.scale_x);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_y,
					 constrained_preserve.plan->Candidate().state.scale_y);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		constrained_preserve.plan->Candidate().state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));

	auto const constrained_fit = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, scaled.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(constrained_fit)
		<< DescribePerspectivePlanError(constrained_fit.error);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		constrained_fit.plan->Candidate().state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
}

TEST(perspective_apply_plan, fax_frz_only_writes_and_verifies_affine_geometry) {
	ApplyFixture fixture;
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\org(120,90)\\fay0.07\\frx8\\fry-6\\p1}"
		"m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);

	auto desired = capture.source->forward_input;
	desired.state.position = {360.0, 250.0};
	desired.state.origin.reset();
	desired.state.scale_x = 125.0;
	desired.state.scale_y = 82.0;
	desired.state.shear_x = 0.24;
	desired.state.shear_y = 0.0;
	desired.state.rotation_x = 0.0;
	desired.state.rotation_y = 0.0;
	desired.state.rotation_z = -13.0;
	auto const target = ForwardQuad(desired);
	ASSERT_TRUE(target) << DescribeForwardError(target.error);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	ASSERT_TRUE(planned.plan);
	EXPECT_EQ(CandidateFamily::AffineFax, planned.plan->Family());
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		planned.plan->Candidate().state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	auto const& replacement = planned.plan->ReplacementText();
	EXPECT_NE(std::string::npos, replacement.find("\\fax"));
	EXPECT_NE(std::string::npos, replacement.find("\\frz"));
	EXPECT_EQ(std::string::npos, replacement.find("\\fay"));
	EXPECT_EQ(std::string::npos, replacement.find("\\frx"));
	EXPECT_EQ(std::string::npos, replacement.find("\\fry"));
	EXPECT_EQ(std::string::npos, replacement.find("\\org"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		recaptured.source->state.transform,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target.quad,
		fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
}

TEST(perspective_apply_plan, fax_frz_only_can_preserve_scale_for_affine_targets) {
	ApplyFixture fixture;
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\fscx123.456789\\fscy87.654321\\p1}"
		"m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);

	auto desired = capture.source->forward_input;
	desired.state.position = {350.0, 245.0};
	desired.state.shear_x = 0.21;
	desired.state.rotation_z = -11.0;
	auto const target = ForwardQuad(desired);
	ASSERT_TRUE(target) << DescribeForwardError(target.error);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Preserve,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_x,
		planned.plan->Candidate().state.scale_x);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_y,
		planned.plan->Candidate().state.scale_y);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		planned.plan->Candidate().state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\fscx123.456789"));
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\fscy87.654321"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_x,
		recaptured.source->state.transform.scale_x);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_y,
		recaptured.source->state.transform.scale_y);
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target.quad,
		fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
}

TEST(perspective_apply_plan, fax_frz_only_settles_for_the_nearest_affine_on_a_projective_target) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	auto desired = capture.source->forward_input;
	desired.state.position = {360.0, 250.0};
	desired.state.scale_x = 118.0;
	desired.state.scale_y = 92.0;
	desired.state.shear_x = 0.16;
	desired.state.rotation_x = 21.0;
	desired.state.rotation_y = -17.0;
	desired.state.rotation_z = 13.0;
	auto const target = ForwardQuad(desired);
	ASSERT_TRUE(target) << DescribeForwardError(target.error);

	auto const automatic = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target.quad);
	ASSERT_TRUE(automatic) << DescribePerspectivePlanError(automatic.error);
	EXPECT_TRUE(
		automatic.plan->Family() == CandidateFamily::ProjectiveImplicitFax
		|| automatic.plan->Family() == CandidateFamily::ProjectiveImplicitFay
		|| automatic.plan->Family() == CandidateFamily::ProjectiveExplicitOrigin);

	auto const constrained = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(constrained) << DescribePerspectivePlanError(constrained.error);
	// The switch cannot express the projective target, so the plan settles
	// for the nearest affine and reports the shortfall instead of refusing.
	EXPECT_TRUE(constrained.plan->Snapped());
	EXPECT_GT(constrained.plan->SnapError(), 0.1);
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		constrained.plan->Candidate().state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));
}

// The review's repro: a line carrying a high-precision restricted tag, a pure
// translation, and a 4-decimal cap. Quantization used to round the kept
// \frx12.123456 down to 12.12346 and the staged re-verification refused the
// plan with StagedRepresentationMismatch; the kept tags now survive byte for
// byte and the plan applies.
TEST(perspective_apply_plan, fax_frz_only_keeps_a_high_precision_restricted_tag_byte_exact) {
	ApplyFixture fixture;
	fixture.line->Text =
		R"({\an7\pos(100,100)\frx12.123456\p1}m 0 0 l 100 0 100 50 0 50)";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_DOUBLE_EQ(12.123456, capture.source->state.transform.rotation_x);

	auto const target = Translate(CurrentQuad(capture), 20.0, 0.0);
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly, 4);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_DOUBLE_EQ(
		capture.source->state.transform.rotation_x,
		planned.plan->Candidate().state.rotation_x);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find(R"(\frx12.123456)"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	EXPECT_DOUBLE_EQ(12.123456, recaptured.source->state.transform.rotation_x);
}

// The multiline companion to the frx case: a pure translation of a two-line
// text with high-precision shear must keep both source tags byte for byte at
// every decimal setting -- at zero decimals the old chain quantized the
// frozen layout shear to zero and rejected the plan outright.
TEST(perspective_apply_plan, multiline_shear_survives_a_zero_decimal_translation) {
	ApplyFixture fixture;
	fixture.line->Text =
		R"({\an7\pos(100,100)\fax0.123456\fay0.234567}HHH\NHHH)";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_DOUBLE_EQ(0.123456, capture.source->state.transform.shear_x);
	ASSERT_DOUBLE_EQ(0.234567, capture.source->state.transform.shear_y);

	auto const target = Translate(CurrentQuad(capture), 20.0, 0.0);
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::Automatic, 0);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_DOUBLE_EQ(0.123456, planned.plan->Candidate().state.shear_x);
	EXPECT_DOUBLE_EQ(0.234567, planned.plan->Candidate().state.shear_y);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find(R"(\fax0.123456\fay0.234567)"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	EXPECT_DOUBLE_EQ(0.123456, recaptured.source->state.transform.shear_x);
	EXPECT_DOUBLE_EQ(0.234567, recaptured.source->state.transform.shear_y);
}

TEST(perspective_apply_plan, translation_preserves_sheared_plane_geometry) {
	for (auto const *tags : {R"(\fax0.2\frx30)",
							 R"(\fax0.2\fay0.1\frx30\fry-15)"}) {
		SCOPED_TRACE(tags);
		ApplyFixture fixture;
		fixture.line->Text = std::string(R"({\an7\pos(100,100))") + tags + R"(\p1}m 0 0 l 200 0 200 80 0 80)";
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
		auto const target = Translate(CurrentQuad(capture), 20.0, 0.0);
		auto const planned = BuildPerspectiveMutationPlan(
			fixture.file, fixture.context, *capture.source, target, 0.1);
		ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
		EXPECT_FALSE(planned.plan->Snapped());

		auto const executed = ExecutePerspectiveMutationPlan(
			fixture.file, fixture.context, *planned.plan);
		ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
		auto const recaptured = fixture.Capture();
		ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
		auto const actual = CurrentQuad(recaptured);
		for (std::size_t index = 0; index < actual.size(); ++index) {
			EXPECT_LE(std::hypot(
						  (actual[index].x - target[index].x) * fixture.context.output_mapping.scale_x,
						  (actual[index].y - target[index].y) * fixture.context.output_mapping.scale_y),
					  0.1);
		}
	}
}

TEST(perspective_apply_plan, multiline_shear_allows_new_plane_rotations) {
	for (auto const tilt : {Vec2{.x = 30.0, .y = 0.0}, Vec2{.x = 0.0, .y = -20.0}}) {
		SCOPED_TRACE(FormatAssPoint(tilt, 1));
		ApplyFixture fixture;
		fixture.line->Text = R"({\an7\pos(100,100)\fax0.2\fay0.1}HHH\NHHH)";
		auto const capture = CapturePerspectiveSource(
			fixture.file, *fixture.line, fixture.context, &FixedTextExtents);
		ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
		auto desired = capture.source->forward_input;
		desired.state.rotation_x = tilt.x;
		desired.state.rotation_y = tilt.y;
		auto const target = ForwardQuad(desired);
		ASSERT_TRUE(target);
		auto const planned = BuildPerspectiveMutationPlan(
			fixture.file, fixture.context, *capture.source, target.quad, 0.1,
			&FixedTextExtents);
		ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
		EXPECT_FALSE(planned.plan->Snapped());

		auto const executed = ExecutePerspectiveMutationPlan(
			fixture.file, fixture.context, *planned.plan);
		ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
		auto const recaptured = CapturePerspectiveSource(
			fixture.file, *fixture.line, fixture.context, &FixedTextExtents);
		ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
		EXPECT_DOUBLE_EQ(0.2, recaptured.source->state.transform.shear_x);
		EXPECT_DOUBLE_EQ(0.1, recaptured.source->state.transform.shear_y);
		auto const actual = CurrentQuad(recaptured);
		for (std::size_t index = 0; index < actual.size(); ++index) {
			EXPECT_LE(std::hypot(
						  (actual[index].x - target.quad[index].x) * fixture.context.output_mapping.scale_x,
						  (actual[index].y - target.quad[index].y) * fixture.context.output_mapping.scale_y),
					  0.1);
		}
	}
}

TEST(perspective_apply_plan, a_no_change_plan_keeps_the_solver_reason_empty) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	auto desired = capture.source->forward_input;
	desired.state.position = {360.0, 250.0};
	desired.state.scale_x = 118.0;
	desired.state.scale_y = 92.0;
	desired.state.shear_x = 0.16;
	desired.state.rotation_x = 21.0;
	desired.state.rotation_y = -17.0;
	desired.state.rotation_z = 13.0;
	auto const target = ForwardQuad(desired);
	ASSERT_TRUE(target) << DescribeForwardError(target.error);

	// A plan that fails before the solver runs keeps the reason empty: a
	// no-change target must not inherit a previous solve's classification.
	auto const no_change = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, CurrentQuad(capture));
	EXPECT_EQ(PerspectivePlanError::NoChange, no_change.error);
	EXPECT_EQ(NoFeasibleReason::None, no_change.solver_no_feasible_reason);
}

// The UI draws ResultQuad() as the reachable shape and reports SnapError() as
// the shortfall, so both have to be true of the same plan: a target the
// restricted subset cannot express must still produce a plan, and that plan's
// own geometry must be where the emitted tags actually land.
TEST(perspective_apply_plan, a_dragged_corner_reports_the_reachable_quad_it_settled_for) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	// The realistic gesture: a quad that was a parallelogram until one corner
	// was dragged a few pixels off it. That tiny twist leaves the affine
	// reachable set, so the plan settles for the nearest affine and reports
	// the shortfall instead of refusing the drag.
	auto target = CurrentQuad(capture);
	target[2].x += 4.0;
	target[2].y += 2.0;

	auto const snapped = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly,
		kDefaultPerspectiveDecimalPlaces);
	ASSERT_TRUE(snapped) << DescribePerspectivePlanError(snapped.error);
	EXPECT_TRUE(snapped.plan->Snapped());
	EXPECT_GT(snapped.plan->SnapError(), 0.0);
	// The twist is a couple of script pixels; the nearest affine must land
	// within that neighbourhood rather than somewhere unrelated.
	EXPECT_LE(snapped.plan->SnapError(), 8.0);

	// ResultQuad is re-evaluated from the staged line, so it must sit where the
	// solver said it would rather than on the unreachable target.
	double result_to_target = 0.0;
	auto const& result = snapped.plan->ResultQuad();
	for (std::size_t index = 0; index < result.size(); ++index) {
		result_to_target = std::max(result_to_target, std::hypot(
			result[index].x - target[index].x,
			result[index].y - target[index].y));
	}
	EXPECT_GT(result_to_target, 0.0);
	// MaxError bounds total deviation from the drawn quad in output pixels; the
	// per-corner distance here is in script pixels, so this is a sanity bound,
	// not an identity.
	EXPECT_LE(result_to_target, 8.0);
}

TEST(perspective_apply_plan, generated_tags_honor_maximum_decimals) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 12.345, -8.765), 0.1,
		nullptr, PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::Automatic,
		kDefaultPerspectiveDecimalPlaces);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	ExpectSerializedAtMost(planned.plan->Candidate().serialized, 2);
	auto const& replacement = planned.plan->ReplacementText();
	EXPECT_NE(std::string::npos, replacement.find(planned.plan->Candidate().serialized.position));
}

TEST(perspective_apply_plan, preserve_scale_round_trips_inline_precision) {
	ApplyFixture fixture;
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\fscx123.456789\\fscy87.654321\\p1}"
		"m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	EXPECT_DOUBLE_EQ(123.456789, capture.source->state.transform.scale_x);
	EXPECT_DOUBLE_EQ(87.654321, capture.source->state.transform.scale_y);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 15.0, -8.0), 0.1,
		nullptr, PerspectiveScalePolicy::Preserve);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\fscx123.456789"));
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\fscy87.654321"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	EXPECT_DOUBLE_EQ(
		capture.source->state.transform.scale_x,
		recaptured.source->state.transform.scale_x);
	EXPECT_DOUBLE_EQ(
		capture.source->state.transform.scale_y,
		recaptured.source->state.transform.scale_y);
}

// A scale lock holds scale exactly, at every decimal-places setting, because
// Preserve never re-serializes \fscx/\fscy -- it copies the source tags through
// untouched. Decimal places bound the tags this tool *writes*, so they must not
// be able to perturb a scale it was told to leave alone. Locks the whole chain:
// solver freeze, MakeRewriteDelta's exact check, and verbatim tag copying.
TEST(perspective_apply_plan, preserve_scale_is_exact_at_every_decimal_setting) {
	ApplyFixture fixture;
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\fscx123.456789\\fscy87.654321\\p1}"
		"m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);

	// A pure translation, so scale is the only thing a Preserve policy has to
	// hold; any rejection here is about the emitted digits and nothing else.
	auto const target = Translate(CurrentQuad(capture), 15.0, -8.0);
	for (int decimals = kMinPerspectiveDecimalPlaces;
		decimals <= kMaxPerspectiveDecimalPlaces; ++decimals) {
		auto const planned = BuildPerspectiveMutationPlan(
			fixture.file, fixture.context, *capture.source, target, 0.1,
			nullptr, PerspectiveScalePolicy::Preserve,
			PerspectiveRepresentationPolicy::Automatic, decimals);
		ASSERT_TRUE(planned)
			<< "decimals=" << decimals << " "
			<< DescribePerspectivePlanError(planned.error);
		EXPECT_NE(PerspectivePlanError::StagedScaleMismatch, planned.error)
			<< "decimals=" << decimals;

		// Bit-exact, not within a rounding step: the full source precision has
		// to survive even when the setting allows fewer digits than it needs.
		auto const& state = planned.plan->Candidate().state;
		EXPECT_DOUBLE_EQ(
			capture.source->state.transform.scale_x, state.scale_x)
			<< "decimals=" << decimals;
		EXPECT_DOUBLE_EQ(
			capture.source->state.transform.scale_y, state.scale_y)
			<< "decimals=" << decimals;
		// The original six-decimal tags, not a re-serialized copy of them.
		EXPECT_NE(std::string::npos,
			planned.plan->ReplacementText().find("\\fscx123.456789"))
			<< "decimals=" << decimals;
		EXPECT_NE(std::string::npos,
			planned.plan->ReplacementText().find("\\fscy87.654321"))
			<< "decimals=" << decimals;

		// A fresh fixture, not a copy: ApplyFixture owns an AssFile and a raw
		// line pointer into it, so copying would leave the pointer dangling.
		ApplyFixture probe;
		probe.line->Text = fixture.line->Text.get();
		auto const probe_capture = probe.Capture();
		ASSERT_TRUE(probe_capture)
			<< DescribePerspectivePlanError(probe_capture.error);
		auto const probe_plan = BuildPerspectiveMutationPlan(
			probe.file, probe.context, *probe_capture.source, target, 0.1,
			nullptr, PerspectiveScalePolicy::Preserve,
			PerspectiveRepresentationPolicy::Automatic, decimals);
		ASSERT_TRUE(probe_plan) << DescribePerspectivePlanError(probe_plan.error);
		ASSERT_TRUE(ExecutePerspectiveMutationPlan(
			probe.file, probe.context, *probe_plan.plan));
		auto const applied = probe.Capture();
		ASSERT_TRUE(applied) << DescribePerspectivePlanError(applied.error);
		EXPECT_DOUBLE_EQ(
			capture.source->state.transform.scale_x,
			applied.source->state.transform.scale_x) << "decimals=" << decimals;
		EXPECT_DOUBLE_EQ(
			capture.source->state.transform.scale_y,
			applied.source->state.transform.scale_y) << "decimals=" << decimals;
	}
}

TEST(perspective_apply_plan, vertical_font_keeps_semantic_quarter_turn_on_apply) {
	ApplyFixture fixture;
	fixture.file.Styles.front().font = "@Vertical Test";
	fixture.line->Text = "{\\an7\\pos(100,100)\\fay-2\\frz270}ABCD";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source->current_quad);
	auto const current_edge = (*capture.source->current_quad)[1]
		- (*capture.source->current_quad)[0];
	EXPECT_GT(
		std::abs(current_edge.x * fixture.context.output_mapping.scale_x),
		std::abs(current_edge.y * fixture.context.output_mapping.scale_y));
	auto const semantic_direction = PerspectiveFirstEdgeDirection(*capture.source);
	ASSERT_TRUE(semantic_direction);
	EXPECT_NEAR(0.0, semantic_direction->x, 1.0e-12);
	EXPECT_NEAR(1.5, semantic_direction->y, 1.0e-12);
	auto extreme_mapping = *capture.source;
	extreme_mapping.fingerprint.context.output_mapping = {1.0e9, 1.0e-9};
	auto const stable_cardinal = PerspectiveFirstEdgeDirection(extreme_mapping);
	ASSERT_TRUE(stable_cardinal);
	EXPECT_DOUBLE_EQ(0.0, stable_cardinal->x);
	EXPECT_DOUBLE_EQ(1.0e-9, stable_cardinal->y);

	auto target = PerspectiveQuadFromOppositeCorners(
		{165.0, 25.0}, {270.0, 325.0},
		semantic_direction);
	target[1].x -= 20.0;
	target[2].y -= 25.0;
	target[3] = target[3] + Vec2 {15.0, 20.0};
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\frz270"));
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\fay"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target,
		fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
}

TEST(perspective_apply_plan, vertical_font_can_redraw_after_current_projection_fails) {
	ApplyFixture fixture;
	fixture.file.Styles.front().font = "@Vertical Test";
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\fscy1000\\frx90\\frz270}ABCD";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_FALSE(capture.source->current_quad);
	EXPECT_EQ(ForwardError::InvalidQuad,
		ForwardQuad(capture.source->forward_input).error);
	auto const direction = PerspectiveFirstEdgeDirection(*capture.source);
	ASSERT_TRUE(direction);
	EXPECT_NEAR(0.0, direction->x, 1.0e-12);
	EXPECT_NEAR(1.5, direction->y, 1.0e-12);

	auto const target = PerspectiveQuadFromOppositeCorners(
		{240.0, 160.0}, {400.0, 224.0}, direction);
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\frz270"));
	EXPECT_EQ(std::string::npos,
		planned.plan->ReplacementText().find("\\org"));

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	ASSERT_TRUE(recaptured.source->current_quad);
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target,
		fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
}

TEST(perspective_apply_plan, tilted_vertical_font_keeps_complete_z_rotation) {
	ApplyFixture fixture;
	fixture.file.Styles.front().font = "@Vertical Test";
	fixture.line->Text = "{\\an7\\pos(100,100)\\frz280}ABCD";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	auto const direction = PerspectiveFirstEdgeDirection(*capture.source);
	ASSERT_TRUE(direction);
	EXPECT_GT(std::abs(direction->y), std::abs(direction->x));
	auto const target = PerspectiveQuadFromOppositeCorners(
		{240.0, 120.0}, {400.0, 300.0}, direction);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\frz280"));
}

TEST(perspective_apply_plan, vertical_font_style_does_not_lock_ass_drawings) {
	ApplyFixture fixture;
	fixture.file.Styles.front().font = "@Vertical Test";
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\frz270\\p1}m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source->state.drawing_mode);

	Quad const target {{{240.0, 160.0}, {400.0, 160.0},
		{400.0, 224.0}, {240.0, 224.0}}};
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_EQ(std::string::npos,
		planned.plan->ReplacementText().find("\\frz270"));
}

TEST(perspective_apply_plan, vertical_font_normalizes_out_of_range_z_on_redraw) {
	ApplyFixture fixture;
	fixture.file.Styles.front().font = "@Vertical Test";
	fixture.line->Text = "{\\an7\\pos(100,100)\\frz100000000}ABCD";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_FALSE(capture.source->current_quad);
	EXPECT_EQ(ForwardError::TransformParameterOutOfRange,
		ForwardQuad(capture.source->forward_input).error);
	auto const direction = PerspectiveFirstEdgeDirection(*capture.source);
	ASSERT_TRUE(direction);
	EXPECT_GT(std::abs(direction->y), std::abs(direction->x));
	auto const target = PerspectiveQuadFromOppositeCorners(
		{240.0, 120.0}, {400.0, 300.0}, direction);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(std::string::npos,
		planned.plan->ReplacementText().find("\\frz280"));
}

TEST(perspective_apply_plan, ordinary_edge_direction_uses_output_pixel_aspect) {
	ApplyFixture fixture;
	auto capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	capture.source->fingerprint.context.output_mapping = {1.0, 2.0};
	capture.source->current_quad = Quad {{{100.0, 100.0}, {110.0, 109.0},
		{101.0, 119.0}, {91.0, 110.0}}};
	ASSERT_TRUE(ValidateQuad(*capture.source->current_quad));

	auto const direction = PerspectiveFirstEdgeDirection(*capture.source);
	ASSERT_TRUE(direction);
	EXPECT_DOUBLE_EQ(10.0, direction->x);
	EXPECT_DOUBLE_EQ(18.0, direction->y);
	auto const target = PerspectiveQuadFromOppositeCorners(
		{2.0, 3.0}, {9.0, 8.0}, direction);
	EXPECT_DOUBLE_EQ(9.0, target[0].x);
	EXPECT_DOUBLE_EQ(3.0, target[0].y);
	EXPECT_DOUBLE_EQ(9.0, target[1].x);
	EXPECT_DOUBLE_EQ(8.0, target[1].y);
}

TEST(perspective_apply_plan, external_geometry_recapture_keeps_target_with_fresh_source) {
	ApplyFixture fixture;
	fixture.line->Text = "ordinary text";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source->current_quad);
	auto const target = Translate(*capture.source->current_quad, 23.0, -11.0);
	PerspectiveQuadEditState state;
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.Bind(capture.source->current_quad, true));
	ASSERT_EQ(PerspectiveQuadEditError::None, state.ReplaceTarget(target));

	fixture.line->Text = "{\\fscx120}ordinary text";
	EXPECT_FALSE(MatchesPerspectiveSource(
		fixture.file, fixture.context, capture.source->fingerprint));
	EXPECT_EQ(PerspectivePlanError::StaleSource,
		BuildPerspectiveMutationPlan(
			fixture.file, fixture.context, *capture.source, target, 0.1,
			FixedTextExtents).error);
	auto const recaptured = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	ASSERT_TRUE(recaptured.source->current_quad);
	EXPECT_NE(
		(*capture.source->current_quad)[1].x - (*capture.source->current_quad)[0].x,
		(*recaptured.source->current_quad)[1].x - (*recaptured.source->current_quad)[0].x);
	ASSERT_EQ(PerspectiveQuadEditError::None,
		state.RefreshCurrent(recaptured.source->current_quad, true));
	ASSERT_TRUE(state.Current());
	ASSERT_TRUE(state.Target());
	for (std::size_t index = 0; index < target.size(); ++index) {
		EXPECT_DOUBLE_EQ(
			(*recaptured.source->current_quad)[index].x, (*state.Current())[index].x);
		EXPECT_DOUBLE_EQ(
			(*recaptured.source->current_quad)[index].y, (*state.Current())[index].y);
		EXPECT_DOUBLE_EQ(target[index].x, (*state.Target())[index].x);
		EXPECT_DOUBLE_EQ(target[index].y, (*state.Target())[index].y);
	}
	EXPECT_TRUE(state.IsModified());

	auto const refreshed_plan = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *recaptured.source, *state.Target(), 0.1,
		FixedTextExtents);
	ASSERT_TRUE(refreshed_plan)
		<< DescribePerspectivePlanError(refreshed_plan.error);
}

TEST(perspective_apply_plan, capture_and_apply_do_not_require_a_current_quad) {
	ApplyFixture fixture;
	fixture.line->Text = "{\\an7\\pos(100,100)\\fscx0}AB";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_FALSE(capture.source->current_quad);
	EXPECT_EQ(AssApplyBlocker::None, capture.source->apply_blocker);
	EXPECT_EQ(BoundsKind::Text, capture.source->forward_input.bounds.kind);
	EXPECT_EQ(ForwardError::DegenerateScale,
		ForwardQuad(capture.source->forward_input).error);

	Quad const target {{{240.0, 160.0}, {400.0, 160.0},
		{400.0, 224.0}, {240.0, 224.0}}};
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(CandidateFamily::NoOp, planned.plan->Family());
	EXPECT_NE(CandidateFamily::CurrentRepresentation, planned.plan->Family());
	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	auto const recaptured = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	ASSERT_TRUE(recaptured.source->current_quad);
	auto const residual = MeasurePerspectiveResidual(
		recaptured.source->forward_input, target, fixture.context.output_mapping);
	ASSERT_TRUE(residual) << DescribeResidualError(residual.error);
	EXPECT_LE(residual.max_error, 0.1);
}

TEST(perspective_apply_plan, staged_text_reflow_is_rejected_before_mutation) {
	ApplyFixture fixture;
	fixture.line->Text = "AB";
	auto const capture = CapturePerspectiveSource(
		fixture.file, *fixture.line, fixture.context, FixedTextExtents);
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source->current_quad);
	auto const original_text = fixture.line->Text.get();

	// The 640-wide PlayRes has 610 pixels after the 10/20 style margins.
	// Expanding a 20-pixel base line to 620 pixels would make the renderer
	// choose automatic wrapping, so the fixed-base-rectangle solver must fail
	// closed rather than write tags which only pass its own stale bounds.
	Quad const target {{{10.0, 100.0}, {630.0, 100.0},
		{630.0, 120.0}, {10.0, 120.0}}};
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1,
		FixedTextExtents);
	EXPECT_EQ(PerspectivePlanError::StagedBoundsEvaluationFailed, planned.error);
	EXPECT_EQ(AssBoundsError::UnsupportedAutomaticWrap, planned.bounds_error);
	EXPECT_FALSE(planned.plan);
	EXPECT_EQ(original_text, fixture.line->Text.get());
}

TEST(perspective_apply_plan, rejects_invalid_blocked_and_no_change_targets_without_mutation) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture);
	auto const original_text = fixture.line->Text.get();

	auto invalid = CurrentQuad(capture);
	invalid[1] = invalid[0];
	auto const invalid_plan = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, invalid);
	EXPECT_EQ(PerspectivePlanError::InvalidTarget, invalid_plan.error);
	EXPECT_FALSE(invalid_plan.plan);

	auto const no_change = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, CurrentQuad(capture));
	EXPECT_EQ(PerspectivePlanError::NoChange, no_change.error);
	EXPECT_FALSE(no_change.plan);
	EXPECT_EQ(original_text, fixture.line->Text.get());

	fixture.line->Text =
		"{\\an7\\move(100,100,200,200)\\p1}m 0 0 l 100 0 100 50 0 50";
	auto const blocked_capture = fixture.Capture();
	ASSERT_TRUE(blocked_capture);
	EXPECT_EQ(AssApplyBlocker::UnsupportedMove, blocked_capture.source->apply_blocker);
	auto const blocked_target = Translate(CurrentQuad(blocked_capture), 4.0, 0.0);
	auto const blocked = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *blocked_capture.source, blocked_target);
	EXPECT_EQ(PerspectivePlanError::ApplyBlocked, blocked.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedMove, blocked.apply_blocker);
	EXPECT_EQ(fixture.line->Text.get(), blocked_capture.source->fingerprint.line.text);
}

TEST(perspective_apply_plan, named_style_reset_is_captured_read_only_and_blocks_apply) {
	ApplyFixture fixture;
	auto* added = new AssStyle(DefaultStyleLine());
	added->name = "Alt";
	added->UpdateData();
	fixture.file.Styles.push_back(*added);
	fixture.line->Text = "{\\rAlt}" + DrawingText();

	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);
	ASSERT_TRUE(capture.source);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedNamedReset,
		capture.source->apply_blocker);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedNamedReset,
		capture.apply_blocker);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 4.0, 0.0));
	EXPECT_EQ(PerspectivePlanError::ApplyBlocked, planned.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedNamedReset, planned.apply_blocker);
	EXPECT_FALSE(planned.plan);
}

TEST(perspective_apply_plan, complete_fingerprint_rejects_line_style_script_and_context_changes) {
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		fixture.line->Margin[0] = 45;
		EXPECT_FALSE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
		EXPECT_EQ(PerspectivePlanError::StaleSource,
			BuildPerspectiveMutationPlan(
				fixture.file, fixture.context, *capture.source,
				Translate(CurrentQuad(capture), 1.0, 0.0)).error);
	}
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		// The evaluator reads public style fields before serialized data is updated.
		fixture.file.Styles.front().scalex = 125.0;
		EXPECT_FALSE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		auto* event_style = &fixture.file.Styles.front();
		fixture.file.Styles.erase(fixture.file.Styles.iterator_to(*event_style));
		delete event_style;
		EXPECT_FALSE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		fixture.file.Info.front().SetValue("800");
		EXPECT_FALSE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		auto expect_stale_context = [&](PerspectiveApplyContext changed_context) {
			EXPECT_FALSE(MatchesPerspectiveSource(
				fixture.file, changed_context, capture.source->fingerprint));
			EXPECT_EQ(PerspectivePlanError::StaleSource,
				BuildPerspectiveMutationPlan(
					fixture.file, changed_context, *capture.source,
					Translate(CurrentQuad(capture), 1.0, 0.0)).error);
		};

		auto changed_context = fixture.context;
		++changed_context.frame_number;
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		++changed_context.capture_time_ms;
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		changed_context.play_resolution.width += 1.0;
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		changed_context.layout_resolution.reset();
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		changed_context.layout_resolution->height += 1.0;
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		changed_context.video_storage_resolution->width += 1.0;
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		changed_context.output_mapping.scale_x += 0.01;
		expect_stale_context(changed_context);
		changed_context = fixture.context;
		changed_context.output_mapping.scale_y += 0.01;
		expect_stale_context(changed_context);
	}
}

TEST(perspective_apply_plan, dialogue_fingerprint_covers_all_relevant_event_fields) {
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Text = DrawingText(101, 100); });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Style = "Other"; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Row = 4; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Start = 100; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.End = 4900; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Margin[2] = 20; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Layer = 2; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Comment = true; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Actor = "actor"; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) { line.Effect = "effect"; });
	ExpectStaleAfterLineMutation([](AssDialogue& line) {
		line.ExtradataIds = std::vector<std::uint32_t> {1, 3};
	});
}

TEST(perspective_apply_plan, source_fingerprint_rejects_identical_content_from_another_file) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture);
	auto const original_text = fixture.line->Text.get();
	AssFile other_file(fixture.file);
	ASSERT_FALSE(other_file.Events.empty());
	auto& other_line = other_file.Events.front();
	other_line.Id = fixture.line->Id;
	ASSERT_EQ(fixture.line->Id, other_line.Id);
	ASSERT_EQ(original_text, other_line.Text.get());

	EXPECT_FALSE(MatchesPerspectiveSource(
		other_file, fixture.context, capture.source->fingerprint));
	auto const planned = BuildPerspectiveMutationPlan(
		other_file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 1.0, 0.0));
	EXPECT_EQ(PerspectivePlanError::StaleSource, planned.error);
	EXPECT_FALSE(planned.plan);
	EXPECT_EQ(original_text, fixture.line->Text.get());
	EXPECT_EQ(original_text, other_line.Text.get());
}

TEST(perspective_apply_plan, script_info_fingerprint_preserves_shape_order_and_duplicates) {
	{
		SCOPED_TRACE("added entry");
		ExpectStaleAfterFixtureMutation([](ApplyFixture& fixture) {
			fixture.file.Info.emplace_back("Extra", "value");
		});
	}
	{
		SCOPED_TRACE("removed entry");
		ExpectStaleAfterFixtureMutation([](ApplyFixture& fixture) {
			fixture.file.Info.erase(fixture.file.Info.begin() + 1);
		});
	}
	{
		SCOPED_TRACE("reordered entries");
		ExpectStaleAfterFixtureMutation([](ApplyFixture& fixture) {
			std::swap(fixture.file.Info[0], fixture.file.Info[1]);
		});
	}
	{
		SCOPED_TRACE("duplicate entry");
		ExpectStaleAfterFixtureMutation([](ApplyFixture& fixture) {
			fixture.file.Info.push_back(fixture.file.Info.front());
		});
	}
}

TEST(perspective_apply_plan, style_fingerprint_ignores_unrelated_catalog_changes) {
	{
		SCOPED_TRACE("added style");
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		auto* added = new AssStyle(DefaultStyleLine());
		added->name = "Alt";
		added->UpdateData();
		fixture.file.Styles.push_back(*added);
		EXPECT_TRUE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
	{
		SCOPED_TRACE("removed style");
		ApplyFixture fixture;
		auto* added = new AssStyle(DefaultStyleLine());
		added->name = "Alt";
		added->UpdateData();
		fixture.file.Styles.push_back(*added);
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		fixture.file.Styles.erase(fixture.file.Styles.iterator_to(*added));
		delete added;
		EXPECT_TRUE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
	{
		SCOPED_TRACE("modified style");
		ApplyFixture fixture;
		auto* added = new AssStyle(DefaultStyleLine());
		added->name = "Alt";
		added->UpdateData();
		fixture.file.Styles.push_back(*added);
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		added->scalex = 137.0;
		EXPECT_TRUE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
	{
		SCOPED_TRACE("reordered styles");
		ApplyFixture fixture;
		auto* added = new AssStyle(DefaultStyleLine());
		added->name = "Alt";
		added->UpdateData();
		fixture.file.Styles.push_back(*added);
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		fixture.file.Styles.reverse();
		EXPECT_TRUE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
	}
}

TEST(perspective_apply_plan, context_fingerprint_preserves_video_storage_presence) {
	{
		SCOPED_TRACE("present to absent");
		ExpectStaleAfterFixtureMutation([](ApplyFixture& fixture) {
			fixture.context.video_storage_resolution.reset();
		});
	}

	ApplyFixture fixture;
	fixture.context.video_storage_resolution.reset();
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture);
	auto const original_text = fixture.line->Text.get();
	fixture.context.video_storage_resolution = Resolution {1280.0, 720.0};
	EXPECT_FALSE(MatchesPerspectiveSource(
		fixture.file, fixture.context, capture.source->fingerprint));
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 1.0, 0.0));
	EXPECT_EQ(PerspectivePlanError::StaleSource, planned.error);
	EXPECT_FALSE(planned.plan);
	EXPECT_EQ(original_text, fixture.line->Text.get());
}

TEST(perspective_apply_plan, rejects_same_id_same_content_object_replacement_and_duplicate_ids) {
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		auto* replacement = new AssDialogue(
			static_cast<AssDialogueBase const&>(*fixture.line));
		auto old = fixture.file.iterator_to(*fixture.line);
		fixture.file.Events.erase(old);
		delete fixture.line;
		fixture.line = replacement;
		fixture.file.Events.push_back(*replacement);

		EXPECT_FALSE(MatchesPerspectiveSource(
			fixture.file, fixture.context, capture.source->fingerprint));
		EXPECT_EQ(PerspectivePlanError::StaleSource,
			BuildPerspectiveMutationPlan(
				fixture.file, fixture.context, *capture.source,
				Translate(CurrentQuad(capture), 1.0, 0.0)).error);
	}
	{
		ApplyFixture fixture;
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		fixture.file.Events.push_back(*new AssDialogue(
			static_cast<AssDialogueBase const&>(*fixture.line)));
		auto const duplicate = BuildPerspectiveMutationPlan(
			fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 1.0, 0.0));
		EXPECT_EQ(PerspectivePlanError::DuplicateSourceId, duplicate.error);
	}
}

TEST(perspective_apply_plan, execute_revalidates_after_planning_and_preserves_prepared_text) {
	ApplyFixture fixture;
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture);
	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source,
		Translate(CurrentQuad(capture), 12.0, 8.0));
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	auto const original_text = fixture.line->Text.get();

	fixture.file.Styles.front().angle = 10.0;
	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	EXPECT_EQ(PerspectivePlanError::StaleSource, executed.error);
	EXPECT_EQ(nullptr, executed.line);
	EXPECT_EQ(original_text, fixture.line->Text.get());
	EXPECT_NE(original_text, planned.plan->ReplacementText());
}

TEST(perspective_apply_plan, explicit_transactions_use_exact_line_metadata_and_never_amend) {
	ApplyFixture fixture;
	aegisub::SubtitleCommandSession session(&fixture.file);
	int next_commit_id = 1;
	std::vector<int> amend_inputs;
	std::vector<AssDialogue*> single_lines;
	std::vector<std::vector<AssDialogue const*>> changed_lines;
	auto undo_connection = agi::signal::Connection(fixture.file.AddUndoManager(
		[&](AssFileCommit commit) {
			amend_inputs.push_back(*commit.commit_id);
			single_lines.push_back(commit.single_line);
			changed_lines.emplace_back(
				commit.changed_lines.begin(), commit.changed_lines.end());
			*commit.commit_id = next_commit_id++;
		}));

	std::array<int, 2> commit_ids {};
	for (std::size_t index = 0; index < commit_ids.size(); ++index) {
		auto const capture = fixture.Capture();
		ASSERT_TRUE(capture);
		auto const planned = BuildPerspectiveMutationPlan(
			fixture.file, fixture.context, *capture.source,
			Translate(CurrentQuad(capture), 5.0 + index, 2.0));
		ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
		auto const executed = ExecutePerspectiveMutationPlan(
			fixture.file, fixture.context, *planned.plan);
		ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);

		session.ResetCommitId();
		AssDialogue const* changed[] = {executed.line};
		commit_ids[index] = session.CommitWithFeedback(
			"apply Perspective quad",
			AssFile::COMMIT_DIAG_TEXT,
			-1,
			executed.line,
			changed,
			aegisub::LocalCommitFeedback::ObserveSelf);
		session.ResetCommitId();
		EXPECT_EQ(-1, session.GetCommitId());
	}

	EXPECT_EQ((std::vector<int> {-1, -1}), amend_inputs);
	EXPECT_NE(commit_ids[0], commit_ids[1]);
	ASSERT_EQ(2u, single_lines.size());
	ASSERT_EQ(2u, changed_lines.size());
	for (std::size_t index = 0; index < 2; ++index) {
		EXPECT_EQ(fixture.line, single_lines[index]);
		ASSERT_EQ(1u, changed_lines[index].size());
		EXPECT_EQ(fixture.line, changed_lines[index].front());
	}
}

TEST(perspective_apply_plan, capture_rejects_nonpositive_line_identity) {
	ApplyFixture fixture;
	fixture.line->Id = 0;
	auto const capture = fixture.Capture();
	EXPECT_EQ(PerspectivePlanError::InvalidInput, capture.error);
	EXPECT_FALSE(capture.source);
}

// The end-to-end version of a_scale_lock_does_not_charge_for_size_drift. The
// staged residual is measured against the quad the solver aimed at, so a plan
// that correctly ignored the drawn size is not then rejected for ignoring it.
// Before the effective-target plumbing this returned ResidualExceeded.
TEST(perspective_apply_plan, a_locked_scale_accepts_a_hand_sized_target) {
	ApplyFixture fixture;
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\frz6\\fax0.1\\p1}m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);

	// Moved and enlarged 4%, which is what four hand-placed corners produce.
	// On this 100x50 drawing that is 8 output pixels in x, well past the 4.0
	// The staged re-derivation must accept the shortfall the solver predicted
	// for its own snapped candidate instead of re-charging it as an error.
	auto const current = CurrentQuad(capture);
	Vec2 centre;
	for (auto const& point : current)
		centre = centre + point / 4.0;
	Quad target = current;
	for (auto& point : target) {
		point.x = centre.x + (point.x - centre.x) * 1.04;
		point.y = centre.y + (point.y - centre.y) * 1.04;
	}
	target = Translate(target, 12.0, -7.0);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target, 0.1, nullptr,
		PerspectiveScalePolicy::Preserve,
		PerspectiveRepresentationPolicy::FaxFrzOnly,
		kDefaultPerspectiveDecimalPlaces);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	EXPECT_NE(PerspectivePlanError::ResidualExceeded, planned.error);

	// Scale really was held, so the acceptance did not come from quietly
	// resizing the text after all.
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_x,
		planned.plan->Candidate().state.scale_x);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_y,
		planned.plan->Candidate().state.scale_y);

	// And the result lands at the drawn shape and centre, 4% short in size --
	// which is the honest outcome, not a silent refusal.
	auto const& result = planned.plan->ResultQuad();
	Vec2 result_centre;
	for (auto const& point : result)
		result_centre = result_centre + point / 4.0;
	EXPECT_NEAR(centre.x + 12.0, result_centre.x, 1.0);
	EXPECT_NEAR(centre.y - 7.0, result_centre.y, 1.0);
}

// End-to-end version of the fay-preserving refit: a line that already declares
// \fay, dragged in plane under Preserve + FaxFrzOnly, must come out of Apply
// with its \fay value character-identical to the source's. The fax closed forms
// could only represent this target by zeroing the \fay, which the rewrite would
// then elide -- losing a tag the user wrote for a drag that never asked for it.
TEST(perspective_apply_plan, fax_frz_only_rewrites_a_fay_line_without_touching_the_fay) {
	ApplyFixture fixture;
	fixture.line->Text =
		"{\\an7\\pos(100,100)\\fay0.07\\p1}m 0 0 l 100 0 100 50 0 50";
	auto const capture = fixture.Capture();
	ASSERT_TRUE(capture) << DescribePerspectivePlanError(capture.error);

	// In-plane drag: moved, rotated, and uniformly enlarged. The uniform 5% is
	// size drift, which the scale lock drops by normalizing the affine target;
	// the remaining shape change is a move and a rotation the refit can hit
	// exactly at the source scale.
	auto desired = capture.source->forward_input;
	desired.state.position = {150.0, 140.0};
	desired.state.scale_x = 105.0;
	desired.state.scale_y = 105.0;
	desired.state.rotation_z = 6.0;
	auto const target = ForwardQuad(desired);
	ASSERT_TRUE(target) << DescribeForwardError(target.error);

	auto const planned = BuildPerspectiveMutationPlan(
		fixture.file, fixture.context, *capture.source, target.quad, 0.1,
		nullptr, PerspectiveScalePolicy::Preserve,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(planned) << DescribePerspectivePlanError(planned.error);
	ASSERT_TRUE(planned.plan);
	EXPECT_EQ(CandidateFamily::AffineFay, planned.plan->Family());
	EXPECT_DOUBLE_EQ(0.07, planned.plan->Candidate().state.shear_y);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_x,
		planned.plan->Candidate().state.scale_x);
	EXPECT_DOUBLE_EQ(capture.source->state.transform.scale_y,
		planned.plan->Candidate().state.scale_y);
	// Source-relative, not absolute: the candidate keeps the line's own \fay,
	// which the absolute subset check would reject outright.
	EXPECT_TRUE(MatchesPerspectiveRepresentationPolicy(
		capture.source->state.transform, planned.plan->Candidate().state,
		PerspectiveRepresentationPolicy::FaxFrzOnly));

	// The rewritten \fay is the source value re-emitted character for character.
	auto const& replacement = planned.plan->ReplacementText();
	EXPECT_NE(std::string::npos, replacement.find("\\fay0.07")) << replacement;

	auto const executed = ExecutePerspectiveMutationPlan(
		fixture.file, fixture.context, *planned.plan);
	ASSERT_TRUE(executed) << DescribePerspectivePlanError(executed.error);
	EXPECT_NE(std::string::npos, fixture.line->Text.get().find("\\fay0.07"));
	auto const recaptured = fixture.Capture();
	ASSERT_TRUE(recaptured) << DescribePerspectivePlanError(recaptured.error);
	EXPECT_DOUBLE_EQ(0.07, recaptured.source->state.transform.shear_y);
}
