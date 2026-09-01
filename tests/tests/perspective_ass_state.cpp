#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_style.h"
#include "../../src/perspective_ass_state.h"

#include <string>

namespace {
using namespace perspective;

std::string StyleLine(
	std::string const& name,
	double scale_x = 100.0,
	double scale_y = 100.0,
	double angle = 0.0,
	int alignment = 2) {
	return "Style: " + name
		+ ",Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,"
		+ "-1,0,0,0," + FormatAssNumber(scale_x, 4)
		+ "," + FormatAssNumber(scale_y, 4)
		+ ",0," + FormatAssNumber(angle, 4)
		+ ",1,2,2," + std::to_string(alignment) + ",10,20,30,1";
}

AssStyle* AddStyle(
	AssFile& file,
	std::string const& name,
	double scale_x = 100.0,
	double scale_y = 100.0,
	double angle = 0.0,
	int alignment = 2) {
	auto* style = new AssStyle(StyleLine(name, scale_x, scale_y, angle, alignment));
	file.Styles.push_back(*style);
	return style;
}

AssDialogue MakeLine(std::string const& text, std::string const& style = "Default") {
	AssDialogue line;
	line.Start = 1000;
	line.End = 5000;
	line.Style = style;
	line.Text = text;
	return line;
}

AssStateResult Evaluate(AssFile const& file, AssDialogue const& line, std::int64_t time = 2000) {
	return EvaluateEffectiveAssState({&file, &line, {1920.0, 1080.0}, time});
}

SolverCandidate Candidate(EvaluatedTransformState state) {
	SolverCandidate candidate;
	candidate.state = state;
	candidate.serialized.position = FormatAssPoint(state.position, 4);
	if (state.origin)
		candidate.serialized.origin = FormatAssPoint(*state.origin, 4);
	candidate.serialized.scale_x = FormatAssNumber(state.scale_x, 4);
	candidate.serialized.scale_y = FormatAssNumber(state.scale_y, 4);
	candidate.serialized.shear_x = FormatAssNumber(state.shear_x, 6);
	candidate.serialized.shear_y = FormatAssNumber(state.shear_y, 6);
	candidate.serialized.rotation_x = FormatAssNumber(state.rotation_x, 5);
	candidate.serialized.rotation_y = FormatAssNumber(state.rotation_y, 5);
	candidate.serialized.rotation_z = FormatAssNumber(state.rotation_z, 5);
	return candidate;
}
}

TEST(perspective_ass_state, inherits_geometry_across_blocks_and_applies_axis_overrides_in_order) {
	AssFile file;
	AddStyle(file, "Default");
	auto line = MakeLine(
		"{\\fscx120\\bord3\\xbord5\\shad4\\yshad-2}a"
		"{\\c&H112233&}b");

	auto const result = Evaluate(file, line);
	ASSERT_TRUE(result) << DescribeAssStateError(result.error);
	EXPECT_EQ(2u, result.value.geometry_run_count);
	EXPECT_DOUBLE_EQ(120.0, result.value.transform.scale_x);
	EXPECT_DOUBLE_EQ(5.0, result.value.transform.outline_x);
	EXPECT_DOUBLE_EQ(3.0, result.value.transform.outline_y);
	EXPECT_DOUBLE_EQ(4.0, result.value.transform.shadow_x);
	EXPECT_DOUBLE_EQ(-2.0, result.value.transform.shadow_y);
}

TEST(perspective_ass_state, follows_first_alignment_and_renderer_clamps) {
	AssFile file;
	AddStyle(file, "Default");

	auto const ssa_first = Evaluate(file, MakeLine("{\\a1\\an7}text"));
	ASSERT_TRUE(ssa_first);
	EXPECT_EQ(1, ssa_first.value.transform.alignment);
	EXPECT_DOUBLE_EQ(10.0, ssa_first.value.transform.position.x);

	auto const ass_first = Evaluate(file, MakeLine("{\\an7\\a1}text"));
	ASSERT_TRUE(ass_first);
	EXPECT_EQ(7, ass_first.value.transform.alignment);

	auto const a4 = Evaluate(file, MakeLine("{\\a4}text"));
	ASSERT_TRUE(a4) << DescribeAssStateError(a4.error);
	EXPECT_EQ(7, a4.value.transform.alignment);
	auto const a8 = Evaluate(file, MakeLine("{\\a8}text"));
	ASSERT_TRUE(a8) << DescribeAssStateError(a8.error);
	EXPECT_EQ(7, a8.value.transform.alignment);

	auto const clamped = Evaluate(
		file, MakeLine("{\\bord-2\\xbord-3\\ybord-4"
			"\\xshad-6\\shad-5\\yshad-7}text"));
	ASSERT_TRUE(clamped);
	EXPECT_DOUBLE_EQ(0.0, clamped.value.transform.outline_x);
	EXPECT_DOUBLE_EQ(0.0, clamped.value.transform.outline_y);
	EXPECT_DOUBLE_EQ(0.0, clamped.value.transform.shadow_x);
	EXPECT_DOUBLE_EQ(-7.0, clamped.value.transform.shadow_y);
}

TEST(perspective_ass_state, evaluates_named_resets_read_only_and_supports_bare_resets) {
	AssFile file;
	AddStyle(file, "Default");
	AddStyle(file, "Alt", 125.0, 100.0);

	auto equivalent = MakeLine("{\\fscx125}a{\\rAlt}b");
	auto equivalent_result = Evaluate(file, equivalent);
	ASSERT_TRUE(equivalent_result) << DescribeAssStateError(equivalent_result.error);
	EXPECT_DOUBLE_EQ(125.0, equivalent_result.value.transform.scale_x);
	EXPECT_EQ(AssApplyBlocker::UnsupportedNamedReset, equivalent_result.apply_blocker);
	EXPECT_FALSE(equivalent_result.CanApply());

	auto line_wide = MakeLine("{\\pos(100,200)\\an7\\fscx125}a{\\rAlt}b");
	auto line_wide_result = Evaluate(file, line_wide);
	ASSERT_TRUE(line_wide_result) << DescribeAssStateError(line_wide_result.error);
	EXPECT_DOUBLE_EQ(100.0, line_wide_result.value.transform.position.x);
	EXPECT_EQ(7, line_wide_result.value.transform.alignment);
	EXPECT_EQ(AssApplyBlocker::UnsupportedNamedReset, line_wide_result.apply_blocker);

	auto mixed = MakeLine("{\\fscx125}a{\\r}b");
	auto mixed_result = Evaluate(file, mixed);
	EXPECT_EQ(AssStateError::MixedGeometryRuns, mixed_result.error);

	auto missing = MakeLine("{\\rMissing}text");
	auto missing_result = Evaluate(file, missing);
	EXPECT_EQ(AssStateError::MissingResetStyle, missing_result.error);
	auto recovered = MakeLine("{\\rMissing}{\\r}text");
	auto recovered_result = Evaluate(file, recovered);
	ASSERT_TRUE(recovered_result);
	EXPECT_EQ(AssApplyBlocker::UnsupportedNamedReset, recovered_result.apply_blocker);
	auto trailing = MakeLine("text{\\rMissing}");
	auto trailing_result = Evaluate(file, trailing);
	ASSERT_TRUE(trailing_result);
	EXPECT_EQ(AssApplyBlocker::UnsupportedNamedReset, trailing_result.apply_blocker);

	auto bare = MakeLine("{\\fscx125}{\\r}text");
	auto bare_result = Evaluate(file, bare);
	ASSERT_TRUE(bare_result);
	EXPECT_EQ(AssApplyBlocker::None, bare_result.apply_blocker);
	EXPECT_TRUE(bare_result.CanApply());
	EXPECT_DOUBLE_EQ(100.0, bare_result.value.transform.scale_x);

	auto mixed_font = MakeLine("a{\\fs100}b");
	EXPECT_EQ(AssStateError::MixedGeometryRuns, Evaluate(file, mixed_font).error);

	auto relative_equivalent = MakeLine("{\\fs+10}a{\\fs96}b");
	ASSERT_TRUE(Evaluate(file, relative_equivalent));
	auto relative_mixed = MakeLine("{\\fs10}a{\\fs+10}b");
	EXPECT_EQ(AssStateError::MixedGeometryRuns, Evaluate(file, relative_mixed).error);
}

TEST(perspective_ass_state, evaluates_dynamic_geometry_read_only_and_allows_visual_animation) {
	AssFile file;
	AddStyle(file, "Default");

	auto move = MakeLine("{\\move(0,0,100,100)}text");
	auto move_result = Evaluate(file, move, 2000);
	ASSERT_TRUE(move_result) << DescribeAssStateError(move_result.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedMove, move_result.apply_blocker);
	EXPECT_FALSE(move_result.CanApply());
	EXPECT_DOUBLE_EQ(25.0, move_result.value.transform.position.x);
	EXPECT_DOUBLE_EQ(25.0, move_result.value.transform.position.y);

	auto animated_geometry = MakeLine("{\\t(0,1000,\\frx20)}text");
	auto animated_result = Evaluate(file, animated_geometry, 1500);
	ASSERT_TRUE(animated_result) << DescribeAssStateError(animated_result.error);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedGeometryAnimation,
		animated_result.apply_blocker);
	EXPECT_FALSE(animated_result.CanApply());
	EXPECT_DOUBLE_EQ(10.0, animated_result.value.transform.rotation_x);
	auto accelerated = MakeLine("{\\t(0,1000,2,\\fscx200)}text");
	auto accelerated_result = Evaluate(file, accelerated, 1500);
	ASSERT_TRUE(accelerated_result);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedGeometryAnimation,
		accelerated_result.apply_blocker);
	EXPECT_DOUBLE_EQ(125.0, accelerated_result.value.transform.scale_x);

	auto animated_colour = MakeLine("{\\t(0,1000,\\alpha&H80&)\\fscx120}text");
	auto colour_result = Evaluate(file, animated_colour);
	ASSERT_TRUE(colour_result) << DescribeAssStateError(colour_result.error);
	EXPECT_TRUE(colour_result.CanApply());
	EXPECT_DOUBLE_EQ(120.0, colour_result.value.transform.scale_x);
}

TEST(perspective_ass_state, carries_capture_time_and_rejects_out_of_event_evaluation) {
	AssFile file;
	AddStyle(file, "Default");
	auto line = MakeLine("text");

	auto const result = Evaluate(file, line, 3456);
	ASSERT_TRUE(result);
	EXPECT_EQ(2456, result.value.transform.event_time_ms);
	EXPECT_EQ(AssStateError::InvalidCaptureTime, Evaluate(file, line, 5000).error);
}

TEST(perspective_ass_state, drawing_mode_is_part_of_mixed_run_detection) {
	AssFile file;
	AddStyle(file, "Default");
	file.SetScriptInfo("WrapStyle", "2");
	auto drawing = MakeLine("{\\p1}m 0 0 l 10 10");
	auto drawing_result = Evaluate(file, drawing);
	ASSERT_TRUE(drawing_result);
	EXPECT_TRUE(drawing_result.value.drawing_mode);
	EXPECT_EQ(1, drawing_result.value.drawing_scale);
	auto offset = MakeLine("{\\p1\\pbo12}m 0 0 l 10 10");
	auto offset_result = Evaluate(file, offset);
	ASSERT_TRUE(offset_result);
	EXPECT_DOUBLE_EQ(12.0, offset_result.value.drawing_baseline_offset);
	auto reset = MakeLine("{\\p1\\pbo12\\q3\\r}m 0 0 l 10 10");
	auto reset_result = Evaluate(file, reset);
	ASSERT_TRUE(reset_result) << DescribeAssStateError(reset_result.error);
	EXPECT_TRUE(reset_result.value.drawing_mode);
	EXPECT_EQ(1, reset_result.value.drawing_scale);
	EXPECT_DOUBLE_EQ(12.0, reset_result.value.drawing_baseline_offset);

	auto mixed = MakeLine("text{\\p1}m 0 0 l 10 10");
	EXPECT_EQ(AssStateError::MixedGeometryRuns, Evaluate(file, mixed).error);
	auto mixed_wrap = MakeLine("a{\\q0}b");
	EXPECT_EQ(AssStateError::MixedGeometryRuns, Evaluate(file, mixed_wrap).error);
	auto empty_wrap_reset = MakeLine("{\\q0}a{\\q}b");
	EXPECT_EQ(AssStateError::MixedGeometryRuns, Evaluate(file, empty_wrap_reset).error);
}

TEST(perspective_ass_state, rewrite_preserves_unrelated_tags_and_reapplies_after_resets) {
	EvaluatedTransformState source;
	source.event_time_ms = 2000;
	source.position = {960.0, 540.0};
	source.rotation_z = 12.0;
	auto target = source;
	target.scale_x = 125.0;
	target.scale_y = 90.0;
	target.rotation_z = 20.0;
	auto const candidate = Candidate(target);

	std::string const text =
		"{\\c&H112233&\\fscx100.000\\fr12}A"
		"{\\r\\t(0,100,\\alpha&H80&)\\fscx100}B";
	auto const result = RewritePerspectiveTags(text, source, source, candidate);
	ASSERT_TRUE(result) << DescribeRewriteError(result.error);
	EXPECT_TRUE(result.changed);
	EXPECT_EQ(
		"{\\fscx125\\fscy90\\frz20\\c&H112233&}A"
		"{\\r\\fscx125\\fscy90\\frz20"
		"\\t(0,100,\\alpha&H80&)}B",
		result.text);
}

TEST(perspective_ass_state, rewrite_no_op_is_byte_exact_and_plain_text_gets_one_bundle) {
	EvaluatedTransformState source;
	source.event_time_ms = 2000;
	source.position = {100.0, 200.0};
	std::string const tagged = "{\\c &H112233&\\t(0, 100, \\alpha&H80&)}text";
	auto no_op = RewritePerspectiveTags(tagged, source, source, Candidate(source));
	ASSERT_TRUE(no_op);
	EXPECT_FALSE(no_op.changed);
	EXPECT_EQ(tagged, no_op.text);

	auto target = source;
	target.position = {110.0, 215.0};
	auto rewritten = RewritePerspectiveTags("plain", source, source, Candidate(target));
	ASSERT_TRUE(rewritten);
	EXPECT_EQ("{\\pos(110,215)}plain", rewritten.text);

	target = source;
	target.scale_x = 120.0;
	auto one_axis = RewritePerspectiveTags(
		"{\\c&HFFFFFF&}plain", source, source, Candidate(target));
	ASSERT_TRUE(one_axis);
	EXPECT_EQ("{\\fscx120\\c&HFFFFFF&}plain", one_axis.text);
	auto reset_pair = RewritePerspectiveTags(
		"{\\fsc\\c&HFFFFFF&}plain", source, source, Candidate(target));
	ASSERT_TRUE(reset_pair);
	EXPECT_EQ("{\\fscx120\\c&HFFFFFF&}plain", reset_pair.text);
}

TEST(perspective_ass_state, rewrite_elides_event_style_geometry_only_after_a_change) {
	EvaluatedTransformState event_style;
	event_style.event_time_ms = 2000;
	event_style.alignment = 7;
	event_style.position = {100.0, 200.0};
	event_style.scale_x = 120.0;
	event_style.rotation_z = 15.0;

	auto source = event_style;
	source.scale_x = 100.0;
	source.shear_x = 1.0;
	source.rotation_x = 20.0;
	auto target = event_style;
	auto const rewritten = RewritePerspectiveTags(
		"{\\fscx100\\fax1\\frx20\\c&HFFFFFF&}text",
		source, event_style, Candidate(target));
	ASSERT_TRUE(rewritten) << DescribeRewriteError(rewritten.error);
	EXPECT_TRUE(rewritten.changed);
	EXPECT_EQ("{\\c&HFFFFFF&}text", rewritten.text);

	std::string const redundant = "{\\fscx120\\fax0\\frz15}text";
	auto const no_op = RewritePerspectiveTags(
		redundant, event_style, event_style, Candidate(event_style));
	ASSERT_TRUE(no_op);
	EXPECT_FALSE(no_op.changed);
	EXPECT_EQ(redundant, no_op.text);

	target = event_style;
	target.position.x += 10.0;
	auto const canonicalized = RewritePerspectiveTags(
		redundant, event_style, event_style, Candidate(target));
	ASSERT_TRUE(canonicalized);
	EXPECT_EQ("{\\pos(110,200)}text", canonicalized.text);

	auto const named_reset = RewritePerspectiveTags(
		"{\\fscx100\\fax1\\frx20\\c&HFFFFFF&}a{\\rAlt\\fscx100}b",
		source, event_style, Candidate(event_style));
	EXPECT_EQ(RewriteError::UnsupportedNamedReset, named_reset.error);
}

TEST(perspective_ass_state, rewrite_removes_origin_and_rejects_dynamic_geometry) {
	EvaluatedTransformState source;
	source.event_time_ms = 2000;
	source.position = {100.0, 200.0};
	source.origin = Vec2 {120.0, 220.0};
	auto target = source;
	target.origin.reset();

	auto removed = RewritePerspectiveTags(
		"{\\org(120,220)\\c&HFFFFFF&}text",
		source, source, Candidate(target));
	ASSERT_TRUE(removed);
	EXPECT_EQ("{\\c&HFFFFFF&}text", removed.text);

	target.position.x += 10.0;
	auto move = RewritePerspectiveTags(
		"{\\move(0,0,10,10)}text", source, source, Candidate(target));
	EXPECT_EQ(RewriteError::UnsupportedMove, move.error);
	auto transform = RewritePerspectiveTags(
		"{\\t(0,100,\\fscx120)}text", source, source, Candidate(target));
	EXPECT_EQ(RewriteError::UnsupportedGeometryAnimation, transform.error);
	auto combined = RewritePerspectiveTags(
		"{\\rAlt\\t(0,100,\\fscx120)}text",
		source, source, Candidate(target));
	EXPECT_EQ(RewriteError::UnsupportedGeometryAnimation, combined.error);
	auto later_move = RewritePerspectiveTags(
		"{\\rAlt}text{\\move(0,0,10,10)}",
		source, source, Candidate(target));
	EXPECT_EQ(RewriteError::UnsupportedMove, later_move.error);
	auto nested_move = RewritePerspectiveTags(
		"{\\t(0,100,\\move(0,0,10,10))}text",
		source, source, Candidate(target));
	EXPECT_EQ(RewriteError::UnsupportedMove, nested_move.error);

	AssFile file;
	AddStyle(file, "Default");
	auto malformed_origin = MakeLine("{\\org(120,bad)}text");
	EXPECT_EQ(
		AssStateError::InvalidGeometryParameter,
		Evaluate(file, malformed_origin).error);

	auto mismatched_time = target;
	mismatched_time.event_time_ms += 1;
	auto mismatch = RewritePerspectiveTags(
		"{\\c&HFFFFFF&}text", source, source, Candidate(mismatched_time));
	EXPECT_EQ(RewriteError::InvalidTarget, mismatch.error);
}

TEST(perspective_ass_state, preserve_scale_keeps_original_scale_tags_byte_exact) {
	EvaluatedTransformState event_style;
	event_style.event_time_ms = 2000;
	event_style.position = {100.0, 200.0};
	auto source = event_style;
	source.scale_x = 123.456789;
	source.scale_y = 87.654321;
	auto target = source;
	target.position = {110.0, 215.0};

	auto const result = RewritePerspectiveTags(
		"{\\pos(100,200)\\fscx123.456789\\fscy87.654321}text",
		source, event_style, Candidate(target),
		PerspectiveScalePolicy::Preserve);
	ASSERT_TRUE(result) << DescribeRewriteError(result.error);
	EXPECT_EQ(
		"{\\pos(110,215)\\fscx123.456789\\fscy87.654321}text",
		result.text);

	target = source;
	target.shear_x = 0.25;
	auto const reset_runs = RewritePerspectiveTags(
		"{\\fscx123.456789\\fscy87.654321}a"
		"{\\r\\fscx123.456789\\fscy87.654321}b",
		source, event_style, Candidate(target),
		PerspectiveScalePolicy::Preserve);
	ASSERT_TRUE(reset_runs) << DescribeRewriteError(reset_runs.error);
	EXPECT_EQ(
		"{\\fax0.25\\fscx123.456789\\fscy87.654321}a"
		"{\\r\\fax0.25\\fscx123.456789\\fscy87.654321}b",
		reset_runs.text);

	target.scale_x += 1.0;
	auto const changed_scale = RewritePerspectiveTags(
		"{\\fscx123.456789\\fscy87.654321}text",
		source, event_style, Candidate(target),
		PerspectiveScalePolicy::Preserve);
	EXPECT_EQ(RewriteError::InvalidTarget, changed_scale.error);
}

TEST(perspective_ass_state, rewrite_writes_line_wide_bundle_once_and_replays_only_run_state) {
	EvaluatedTransformState source;
	source.event_time_ms = 2000;
	source.position = {100.0, 200.0};
	auto target = source;
	target.position = {110.0, 215.0};
	target.alignment = 7;
	target.scale_x = 120.0;

	auto const result = RewritePerspectiveTags(
		"{\\pos(100,200)\\an2\\fscx100}a{\\rAlt}b",
		source,
		source,
		Candidate(target));
	EXPECT_EQ(RewriteError::UnsupportedNamedReset, result.error);

	auto run_target = source;
	run_target.scale_x = 120.0;
	auto consecutive = RewritePerspectiveTags(
		"{\\rFirst\\rSecond}text", source, source, Candidate(run_target));
	EXPECT_EQ(RewriteError::UnsupportedNamedReset, consecutive.error);
	auto across_blocks = RewritePerspectiveTags(
		"{\\rFirst}{\\rSecond}text", source, source, Candidate(run_target));
	EXPECT_EQ(RewriteError::UnsupportedNamedReset, across_blocks.error);

	auto bare_reset = RewritePerspectiveTags(
		"{\\r}text", source, source, Candidate(run_target));
	ASSERT_TRUE(bare_reset);
	EXPECT_EQ("{\\r\\fscx120}text", bare_reset.text);
}

TEST(perspective_ass_state, spaced_override_spellings_capture_like_libass) {
	// libass skips spaces after the tag backslash (ass_parse.c), so these
	// spellings are real tags; the prototype-table classifier alone called
	// them junk and the capture silently missed every one of them.
	AssFile file;
	AddStyle(file, "Default");

	auto spaced_frz = Evaluate(file, MakeLine(R"({\ frz30})"));
	ASSERT_TRUE(spaced_frz) << DescribeAssStateError(spaced_frz.error);
	EXPECT_EQ(AssApplyBlocker::None, spaced_frz.apply_blocker);
	EXPECT_DOUBLE_EQ(30.0, spaced_frz.value.transform.rotation_z);

	auto spaced_move = Evaluate(file, MakeLine(R"({\ move(0,0,100,0)})"));
	ASSERT_TRUE(spaced_move) << DescribeAssStateError(spaced_move.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedMove, spaced_move.apply_blocker);

	auto spaced_reset = Evaluate(file, MakeLine(R"({\r Alt\frz30})"));
	ASSERT_TRUE(spaced_reset) << DescribeAssStateError(spaced_reset.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedNamedReset, spaced_reset.apply_blocker);

	auto spaced_animation = Evaluate(file, MakeLine(R"({\ t(0,100,\frz30)})"));
	ASSERT_TRUE(spaced_animation) << DescribeAssStateError(spaced_animation.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedGeometryAnimation,
			  spaced_animation.apply_blocker);
}

TEST(perspective_ass_state, rewrite_rejects_spaced_move_and_removes_spaced_frz) {
	EvaluatedTransformState source;
	source.event_time_ms = 2000;
	source.position = {960.0, 540.0};
	source.rotation_z = 30.0; // captured from "\ frz30"
	auto target = source;
	target.rotation_z = 20.0;
	auto const candidate = Candidate(target);

	// The rewrite side classifies the spaced \move too: it must reject the
	// line instead of rewriting tags libass renders differently.
	auto move_rewrite = RewritePerspectiveTags(
		R"({\ move(0,0,100,0)\pos(5,5)})", source, source, candidate);
	EXPECT_EQ(RewriteError::UnsupportedMove, move_rewrite.error);

	// And the spaced \frz is removed with the rest of the family before
	// the composed tag is emitted, not left behind to win last-wins.
	auto const result = RewritePerspectiveTags(
		R"({\ frz30\c&H112233&}A)", source, source, candidate);
	ASSERT_TRUE(result) << DescribeRewriteError(result.error);
	EXPECT_TRUE(result.changed);
	EXPECT_EQ(R"({\frz20\c&H112233&}A)", result.text);
}

// A space after the backslash inside a \t argument region is a nested tag to
// libass (ass_parse.c skips it), so "\t(0,4000,\ frz30)" really animates the
// line. The nested body used to be parsed by the prototype table alone, which
// is space-blind: the evaluator read frz = 0 and no Apply blocker fired, and
// the planner happily judged the animated line statically reachable.
TEST(perspective_ass_state, spaced_nested_animation_tags_evaluate_and_block_apply) {
	AssFile file;
	AddStyle(file, "Default");

	auto const animated = Evaluate(file, MakeLine(R"({\t(0,1000,\ frz30)})"), 1500);
	ASSERT_TRUE(animated) << DescribeAssStateError(animated.error);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedGeometryAnimation,
		animated.apply_blocker);
	EXPECT_FALSE(animated.CanApply());
	EXPECT_DOUBLE_EQ(15.0, animated.value.transform.rotation_z);

	// The scanner only ends a tag name at '(' or '\', so the bytes between
	// the name and its argument region ride along with it; libass matches
	// the tag by prefix and renders every one of these like "\t(...)".
	auto const spaced_paren = Evaluate(
		file, MakeLine(R"({\t (0,1000,\ frz30)})"), 1500);
	ASSERT_TRUE(spaced_paren) << DescribeAssStateError(spaced_paren.error);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedGeometryAnimation,
		spaced_paren.apply_blocker);
	EXPECT_FALSE(spaced_paren.CanApply());
	EXPECT_DOUBLE_EQ(15.0, spaced_paren.value.transform.rotation_z);

	auto const prefixed = Evaluate(
		file, MakeLine(R"({\t1(0,1000,\frz30)})"), 1500);
	ASSERT_TRUE(prefixed) << DescribeAssStateError(prefixed.error);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedGeometryAnimation,
		prefixed.apply_blocker);
	EXPECT_DOUBLE_EQ(15.0, prefixed.value.transform.rotation_z);

	auto const accelerated = Evaluate(
		file, MakeLine(R"({\t(0,1000,2,\ frz30)})"), 1500);
	ASSERT_TRUE(accelerated) << DescribeAssStateError(accelerated.error);
	EXPECT_EQ(
		AssApplyBlocker::UnsupportedGeometryAnimation,
		accelerated.apply_blocker);
	EXPECT_DOUBLE_EQ(7.5, accelerated.value.transform.rotation_z);
}

TEST(perspective_ass_state, rewrite_rejects_spaced_nested_animation) {
	EvaluatedTransformState source;
	source.event_time_ms = 2000;
	source.position = {960.0, 540.0};
	auto const result = RewritePerspectiveTags(
		R"({\t(0,100,\ frz30)}text)", source, source, Candidate(source));
	EXPECT_EQ(RewriteError::UnsupportedGeometryAnimation, result.error);

	auto const spaced_paren = RewritePerspectiveTags(
		R"({\t (0,100,\ frz30)}text)", source, source, Candidate(source));
	EXPECT_EQ(RewriteError::UnsupportedGeometryAnimation, spaced_paren.error);
}

// Under the restricted policy a candidate that keeps the source's \org, \fay,
// \frx and \fry must leave their original spelling byte for byte: the only
// text that reproduces the value bit for bit. Re-serializing through the
// decimal caps used to round \frx12.123456 down to 12.12346 and the staged
// re-verification then refused the very plan the policy allowed.
TEST(perspective_ass_state, fax_frz_only_rewrite_keeps_restricted_tags_byte_exact) {
	// The style baseline carries none of the restricted tags; the line's own
	// overrides do.
	EvaluatedTransformState event_style;
	event_style.event_time_ms = 2000;
	event_style.position = {100.0, 200.0};
	EvaluatedTransformState source = event_style;
	source.rotation_x = 12.123456;
	source.shear_y = 0.123457;
	source.origin = Vec2 {130.0, 230.0};
	auto target = source;
	target.position = {120.0, 220.0};

	auto const kept = RewritePerspectiveTags(
		R"({\org(130,230)\frx12.123456\fay0.123457\c&H112233&}text)",
		source, event_style, Candidate(target),
		PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(kept) << DescribeRewriteError(kept.error);
	EXPECT_TRUE(kept.changed);
	EXPECT_EQ(
		R"({\pos(120,220)\org(130,230)\frx12.123456\fay0.123457\c&H112233&}text)",
		kept.text);

	// The clean form is still the clean form: a candidate that drops the
	// restricted tags has them removed, with nothing re-emitted for them.
	target.origin.reset();
	target.rotation_x = 0.0;
	target.shear_y = 0.0;
	auto const cleaned = RewritePerspectiveTags(
		R"({\org(130,230)\frx12.123456\fay0.123457\c&H112233&}text)",
		source, event_style, Candidate(target),
		PerspectiveScalePolicy::Fit,
		PerspectiveRepresentationPolicy::FaxFrzOnly);
	ASSERT_TRUE(cleaned) << DescribeRewriteError(cleaned.error);
	EXPECT_EQ(R"({\pos(120,220)\c&H112233&}text)", cleaned.text);
}

// A candidate that carries the source's own shear values keeps the original
// \fax/\fay spelling: re-serializing through the decimal caps rounds
// \fax0.123456 down to 0.123 and invalidates a translation that retained the
// source tags. A candidate that does change the fax stays on the ordinary
// remove-and-reemit path, so the elision rule is not over-broad.
TEST(perspective_ass_state, rewrite_keeps_unchanged_source_shear_spelling) {
	EvaluatedTransformState event_style;
	event_style.event_time_ms = 2000;
	event_style.position = {100.0, 200.0};
	EvaluatedTransformState source = event_style;
	source.shear_x = 0.123456;
	source.shear_y = 0.234567;
	auto target = source;
	target.position = {120.0, 200.0};

	auto const kept = RewritePerspectiveTags(
		R"({\fax0.123456\fay0.234567\c&H112233&}text)",
		source, event_style, Candidate(target));
	ASSERT_TRUE(kept) << DescribeRewriteError(kept.error);
	EXPECT_TRUE(kept.changed);
	EXPECT_EQ(
		R"({\pos(120,200)\fax0.123456\fay0.234567\c&H112233&}text)",
		kept.text);

	target.shear_x = 0.25;
	auto const changed = RewritePerspectiveTags(
		R"({\fax0.123456\fay0.234567\c&H112233&}text)",
		source, event_style, Candidate(target));
	ASSERT_TRUE(changed) << DescribeRewriteError(changed.error);
	EXPECT_EQ(
		R"({\pos(120,200)\fax0.25\fay0.234567\c&H112233&}text)",
		changed.text);
}
