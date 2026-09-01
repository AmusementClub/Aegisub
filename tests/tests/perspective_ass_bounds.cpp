#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_style.h"
#include "../../src/perspective_ass_bounds.h"
#include "../../src/perspective_ass_state.h"

#include <algorithm>
#include <limits>
#include <string>

namespace {
using namespace perspective;

AssStyle* AddDefaultStyle(AssFile& file) {
	auto* style = new AssStyle(
		"Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,"
		"-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1");
	file.Styles.push_back(*style);
	return style;
}

AssDialogue MakeLine(std::string const& text) {
	AssDialogue line;
	line.Start = 1000;
	line.End = 5000;
	line.Style = "Default";
	line.Text = text;
	return line;
}

struct EvaluatedLine {
	AssDialogue line;
	AssStateResult state;
};

EvaluatedLine Evaluate(
	AssFile const& file,
	std::string const& text,
	std::int64_t time = 2000,
	Resolution play_resolution = {1920.0, 1080.0}) {
	EvaluatedLine result {MakeLine(text), {}};
	result.state = EvaluateEffectiveAssState(
		{&file, &result.line, play_resolution, time});
	return result;
}

AssBoundsResult Bounds(
	EvaluatedLine const& evaluated,
	AssTextExtentsProvider text_extents = nullptr) {
	if (!evaluated.state)
		return {AssBoundsError::InvalidInput};
	return EvaluateAssBaseBounds(
		{&evaluated.line, &evaluated.state.value, text_extents});
}

bool FixedTextExtents(
	AssStyle*, std::string const& text,
	double& width, double& height, double& descent, double& extlead) {
	width = static_cast<double>(text.size()) * 10.0;
	height = 20.0;
	descent = 4.0;
	extlead = 2.0;
	return true;
}

bool OverrideTextExtents(
	AssStyle* style, std::string const& text,
	double& width, double& height, double& descent, double& extlead) {
	EXPECT_EQ("Perspective Test", style->font);
	EXPECT_DOUBLE_EQ(60.0, style->fontsize);
	EXPECT_DOUBLE_EQ(2.0, style->spacing);
	EXPECT_TRUE(style->bold);
	EXPECT_TRUE(style->italic);
	EXPECT_TRUE(style->underline);
	EXPECT_TRUE(style->strikeout);
	EXPECT_EQ(128, style->encoding);
	EXPECT_DOUBLE_EQ(100.0, style->scalex);
	EXPECT_DOUBLE_EQ(100.0, style->scaley);
	width = static_cast<double>(text.size()) * 10.0;
	height = 20.0;
	descent = 4.0;
	extlead = 2.0;
	return true;
}

bool FailedTextExtents(
	AssStyle*, std::string const&,
	double&, double&, double&, double&) {
	return false;
}

bool InvalidTextExtents(
	AssStyle*, std::string const&,
	double& width, double& height, double& descent, double& extlead) {
	width = std::numeric_limits<double>::infinity();
	height = 20.0;
	descent = 4.0;
	extlead = 2.0;
	return true;
}

ForwardResult Forward(EvaluatedLine const& evaluated) {
	auto const bounds = Bounds(evaluated);
	if (!bounds)
		return {ForwardError::InvalidBounds};
	ForwardInput input;
	input.play_resolution = {1920.0, 1080.0};
	input.bounds = bounds.value;
	input.state = evaluated.state.value.transform;
	return ForwardQuad(input);
}

void ExpectRect(Rect const& value, double left, double top, double right, double bottom) {
	EXPECT_DOUBLE_EQ(left, value.left);
	EXPECT_DOUBLE_EQ(top, value.top);
	EXPECT_DOUBLE_EQ(right, value.right);
	EXPECT_DOUBLE_EQ(bottom, value.bottom);
}

void ExpectExtent(BaseBounds const& value, double width, double height) {
	ASSERT_TRUE(value.alignment_extent);
	EXPECT_DOUBLE_EQ(width, value.alignment_extent->width);
	EXPECT_DOUBLE_EQ(height, value.alignment_extent->height);
}

void ExpectAlignmentOffset(BaseBounds const& value, double x, double y) {
	EXPECT_DOUBLE_EQ(x, value.alignment_offset.x);
	EXPECT_DOUBLE_EQ(y, value.alignment_offset.y);
}

void ExpectResidualSample(BaseBounds const& value, double x, double y) {
	EXPECT_TRUE(std::any_of(
		value.residual_samples.begin(), value.residual_samples.end(),
		[x, y](Vec2 point) { return point.x == x && point.y == y; }));
}

void ExpectYShift(Quad const& baseline, Quad const& shifted, double offset) {
	for (std::size_t index = 0; index < baseline.size(); ++index) {
		EXPECT_DOUBLE_EQ(baseline[index].x, shifted[index].x);
		EXPECT_DOUBLE_EQ(baseline[index].y + offset, shifted[index].y);
	}
}
}

TEST(perspective_ass_bounds, evaluates_drawing_origin_scale_and_baseline_offset) {
	AssFile file;
	AddDefaultStyle(file);

	auto p1 = Evaluate(file, "{\\p1}m 10 20 l 110 20 110 70 10 70");
	auto p1_bounds = Bounds(p1);
	ASSERT_TRUE(p1_bounds) << DescribeAssBoundsError(p1_bounds.error);
	EXPECT_EQ(BoundsKind::Drawing, p1_bounds.value.kind);
	ExpectRect(p1_bounds.value.rectangle, 10.0, 20.0, 110.0, 70.0);
	ExpectExtent(p1_bounds.value, 100.0, 50.0);
	ExpectAlignmentOffset(p1_bounds.value, 0.0, 0.0);

	auto p2 = Evaluate(file, "{\\p2}m 10 20 l 110 20 110 70 10 70");
	auto p2_bounds = Bounds(p2);
	ASSERT_TRUE(p2_bounds) << DescribeAssBoundsError(p2_bounds.error);
	ExpectRect(p2_bounds.value.rectangle, 5.0, 10.0, 55.0, 35.0);
	ExpectExtent(p2_bounds.value, 50.0, 25.0);

	auto positive = Evaluate(file, "{\\p2\\pbo12}m 10 20 l 110 20 110 70 10 70");
	auto positive_bounds = Bounds(positive);
	ASSERT_TRUE(positive_bounds) << DescribeAssBoundsError(positive_bounds.error);
	ExpectRect(positive_bounds.value.rectangle, 5.0, 10.0, 55.0, 35.0);
	ExpectExtent(positive_bounds.value, 50.0, 25.0);

	auto negative = Evaluate(file, "{\\p2\\pbo-12}m 10 20 l 110 20 110 70 10 70");
	auto negative_bounds = Bounds(negative);
	ASSERT_TRUE(negative_bounds) << DescribeAssBoundsError(negative_bounds.error);
	ExpectRect(negative_bounds.value.rectangle, 5.0, 10.0, 55.0, 35.0);
	ExpectExtent(negative_bounds.value, 50.0, 31.0);

	auto above_height = Evaluate(file, "{\\p2\\pbo100}m 10 20 l 110 20 110 70 10 70");
	auto above_height_bounds = Bounds(above_height);
	ASSERT_TRUE(above_height_bounds) << DescribeAssBoundsError(above_height_bounds.error);
	ExpectRect(above_height_bounds.value.rectangle, 5.0, 10.0, 55.0, 35.0);
	ExpectExtent(above_height_bounds.value, 50.0, 50.0);
	ExpectAlignmentOffset(above_height_bounds.value, 0.0, 25.0);
}

TEST(perspective_ass_bounds, baseline_offset_follows_clamped_renderer_line_metrics) {
	AssFile file;
	AddDefaultStyle(file);
	auto const drawing = std::string("m 10 20 l 110 20 110 70 10 70");

	auto bottom = Forward(Evaluate(file, "{\\p2\\an2}" + drawing));
	auto bottom_negative = Forward(Evaluate(file, "{\\p2\\an2\\pbo-12}" + drawing));
	auto bottom_high = Forward(Evaluate(file, "{\\p2\\an2\\pbo100}" + drawing));
	ASSERT_TRUE(bottom);
	ASSERT_TRUE(bottom_negative);
	ASSERT_TRUE(bottom_high);
	ExpectYShift(bottom.quad, bottom_negative.quad, -6.0);
	ExpectYShift(bottom.quad, bottom_high.quad, 0.0);

	auto center = Forward(Evaluate(file, "{\\p2\\an5}" + drawing));
	auto center_negative = Forward(Evaluate(file, "{\\p2\\an5\\pbo-12}" + drawing));
	auto center_high = Forward(Evaluate(file, "{\\p2\\an5\\pbo100}" + drawing));
	ASSERT_TRUE(center);
	ASSERT_TRUE(center_negative);
	ASSERT_TRUE(center_high);
	ExpectYShift(center.quad, center_negative.quad, -3.0);
	ExpectYShift(center.quad, center_high.quad, 12.5);

	auto top = Forward(Evaluate(file, "{\\p2\\an8}" + drawing));
	auto top_negative = Forward(Evaluate(file, "{\\p2\\an8\\pbo-12}" + drawing));
	auto top_high = Forward(Evaluate(file, "{\\p2\\an8\\pbo100}" + drawing));
	ASSERT_TRUE(top);
	ASSERT_TRUE(top_negative);
	ASSERT_TRUE(top_high);
	ExpectYShift(top.quad, top_negative.quad, 0.0);
	ExpectYShift(top.quad, top_high.quad, 25.0);

	auto top_shear = Forward(Evaluate(file, "{\\p2\\an8\\fax0.5}" + drawing));
	auto top_shear_high = Forward(Evaluate(file,
		"{\\p2\\an8\\fax0.5\\pbo100}" + drawing));
	ASSERT_TRUE(top_shear);
	ASSERT_TRUE(top_shear_high);
	for (std::size_t index = 0; index < top_shear.quad.size(); ++index) {
		EXPECT_DOUBLE_EQ(top_shear.quad[index].x, top_shear_high.quad[index].x);
		EXPECT_DOUBLE_EQ(top_shear.quad[index].y + 25.0, top_shear_high.quad[index].y);
	}
}

TEST(perspective_ass_bounds, uses_tight_curve_extrema_and_rejects_multiple_drawing_glyphs) {
	AssFile file;
	AddDefaultStyle(file);

	auto curve = Evaluate(file, "{\\p1}m 0 0 b 0 10 10 10 10 0");
	auto curve_bounds = Bounds(curve);
	ASSERT_TRUE(curve_bounds) << DescribeAssBoundsError(curve_bounds.error);
	ExpectRect(curve_bounds.value.rectangle, 0.0, 0.0, 10.0, 7.5);
	ExpectExtent(curve_bounds.value, 10.0, 10.0);
	ExpectResidualSample(curve_bounds.value, 0.0, 0.0);
	ExpectResidualSample(curve_bounds.value, 0.0, 10.0);
	ExpectResidualSample(curve_bounds.value, 10.0, 10.0);
	ExpectResidualSample(curve_bounds.value, 10.0, 0.0);

	auto scaled_curve = Evaluate(file, "{\\p2}m 0 0 b 0 10 10 10 10 0");
	auto scaled_curve_bounds = Bounds(scaled_curve);
	ASSERT_TRUE(scaled_curve_bounds)
		<< DescribeAssBoundsError(scaled_curve_bounds.error);
	ExpectRect(scaled_curve_bounds.value.rectangle, 0.0, 0.0, 5.0, 3.75);
	ExpectResidualSample(scaled_curve_bounds.value, 0.0, 5.0);
	ExpectResidualSample(scaled_curve_bounds.value, 5.0, 5.0);

	auto contours = Evaluate(file,
		"{\\p1}m 0 0 l 10 0 10 10 0 10 m 100 20 l 110 20 110 30 100 30");
	auto contour_bounds = Bounds(contours);
	ASSERT_TRUE(contour_bounds) << DescribeAssBoundsError(contour_bounds.error);
	ExpectRect(contour_bounds.value.rectangle, 0.0, 0.0, 110.0, 30.0);

	auto split = Evaluate(file,
		"{\\p1}m 10 20 l 110 20{\\c&H112233&}110 70 10 70");
	auto split_bounds = Bounds(split);
	EXPECT_EQ(AssBoundsError::UnsupportedDrawingLayout, split_bounds.error);
	EXPECT_STRNE("unknown Perspective ASS bounds error", DescribeAssBoundsError(split_bounds.error));
}

TEST(perspective_ass_bounds, reports_font_empty_mixed_and_invalid_drawing_diagnostics) {
	AssFile file;
	AddDefaultStyle(file);

	auto text = Evaluate(file, "plain text");
	auto text_bounds = Bounds(text, FixedTextExtents);
	ASSERT_TRUE(text_bounds) << DescribeAssBoundsError(text_bounds.error);
	EXPECT_EQ(BoundsKind::Text, text_bounds.value.kind);
	ExpectRect(text_bounds.value.rectangle, 0.0, 0.0, 100.0, 20.0);

	auto empty = Evaluate(file, "{\\p1}");
	EXPECT_EQ(AssBoundsError::EmptyGeometry, Bounds(empty).error);
	auto whitespace = Evaluate(file, "{\\p1}   ");
	EXPECT_EQ(AssBoundsError::EmptyGeometry, Bounds(whitespace).error);

	auto malformed = Evaluate(file, "{\\p1}m 0 0 l nope");
	EXPECT_EQ(AssBoundsError::InvalidDrawingSyntax, Bounds(malformed).error);

	auto non_finite = Evaluate(file, "{\\p1}m 1e999 0 l 10 10");
	EXPECT_EQ(AssBoundsError::InvalidDrawingSyntax, Bounds(non_finite).error);

	auto spline = Evaluate(file, "{\\p1}m 0 0 s 0 10 10 10 10 0");
	EXPECT_EQ(AssBoundsError::UnsupportedDrawingCommand, Bounds(spline).error);

	auto degenerate = Evaluate(file, "{\\p1}m 0 0 l 10 0");
	auto degenerate_bounds = Bounds(degenerate);
	EXPECT_EQ(AssBoundsError::InvalidDrawingBounds, degenerate_bounds.error);
	EXPECT_EQ(GeometryError::InvalidDomain, degenerate_bounds.geometry_error);

	auto unsafe = Evaluate(file,
		"{\\p1}m 0 0 l 1000000001 0 1000000001 10 0 10");
	auto unsafe_bounds = Bounds(unsafe);
	EXPECT_EQ(AssBoundsError::InvalidDrawingBounds, unsafe_bounds.error);
	EXPECT_EQ(GeometryError::CoordinateOutOfRange, unsafe_bounds.geometry_error);

	auto mixed = Evaluate(file, "text{\\p1}m 0 0 l 10 0 10 10 0 10");
	EXPECT_EQ(AssStateError::MixedGeometryRuns, mixed.state.error);

	auto valid = Evaluate(file, "{\\p1}m 0 0 l 10 0 10 10 0 10");
	ASSERT_TRUE(valid.state);
	auto mismatched = valid.state.value;
	mismatched.geometry_run_count = 2;
	EXPECT_EQ(AssBoundsError::DrawingStateMismatch,
		EvaluateAssBaseBounds({&valid.line, &mismatched}).error);
	EXPECT_EQ(AssBoundsError::InvalidInput, EvaluateAssBaseBounds({}).error);
}

TEST(perspective_ass_bounds, evaluates_uniform_multiline_text_with_effective_font_state) {
	AssFile file;
	AddDefaultStyle(file);

	auto text = Evaluate(file,
		"{\\fnPerspective Test\\fs60\\fsp2\\b1\\i1\\u1\\s1\\fe128}AB\\NXYZ");
	ASSERT_TRUE(text.state) << DescribeAssStateError(text.state.error);
	auto bounds = Bounds(text, OverrideTextExtents);
	ASSERT_TRUE(bounds) << DescribeAssBoundsError(bounds.error);
	EXPECT_EQ(BoundsKind::Text, bounds.value.kind);
	ExpectRect(bounds.value.rectangle, 0.0, 0.0, 30.0, 40.0);
	EXPECT_FALSE(bounds.value.alignment_extent);
	EXPECT_TRUE(bounds.value.residual_samples.empty());

	auto split = Evaluate(file, "A{\\c&H112233&}B");
	ASSERT_TRUE(split.state) << DescribeAssStateError(split.state.error);
	auto split_bounds = Bounds(split, FixedTextExtents);
	ASSERT_TRUE(split_bounds) << DescribeAssBoundsError(split_bounds.error);
	ExpectRect(split_bounds.value.rectangle, 0.0, 0.0, 20.0, 20.0);
}

TEST(perspective_ass_bounds, soft_line_break_follows_wrap_style) {
	AssFile normal_wrap;
	AddDefaultStyle(normal_wrap);
	auto normal = Evaluate(normal_wrap, "A\\nB");
	auto normal_bounds = Bounds(normal, FixedTextExtents);
	ASSERT_TRUE(normal_bounds) << DescribeAssBoundsError(normal_bounds.error);
	ExpectRect(normal_bounds.value.rectangle, 0.0, 0.0, 30.0, 20.0);

	AssFile forced_wrap;
	forced_wrap.Info.emplace_back("WrapStyle", "2");
	AddDefaultStyle(forced_wrap);
	auto forced = Evaluate(forced_wrap, "A\\nB");
	auto forced_bounds = Bounds(forced, FixedTextExtents);
	ASSERT_TRUE(forced_bounds) << DescribeAssBoundsError(forced_bounds.error);
	ExpectRect(forced_bounds.value.rectangle, 0.0, 0.0, 10.0, 40.0);
}

TEST(perspective_ass_bounds, rejects_layouts_which_require_automatic_wrapping) {
	AssFile file;
	AddDefaultStyle(file);

	// PlayResX 100 minus the style's 10/20 margins leaves 70 script pixels.
	auto short_line = Evaluate(file, "ABC", 2000, {100.0, 100.0});
	ASSERT_TRUE(short_line.state);
	EXPECT_DOUBLE_EQ(70.0, short_line.state.value.text_style.available_wrap_width);
	ASSERT_TRUE(Bounds(short_line, FixedTextExtents));

	auto boundary = Evaluate(file, "1234567", 2000, {100.0, 100.0});
	auto boundary_bounds = Bounds(boundary, FixedTextExtents);
	EXPECT_EQ(AssBoundsError::UnsupportedAutomaticWrap, boundary_bounds.error);
	EXPECT_STRNE("unknown Perspective ASS bounds error",
		DescribeAssBoundsError(boundary_bounds.error));

	auto scaled = Evaluate(file, "{\\fscx200}ABCD", 2000, {100.0, 100.0});
	EXPECT_EQ(AssBoundsError::UnsupportedAutomaticWrap,
		Bounds(scaled, FixedTextExtents).error);

	auto no_wrap = Evaluate(file, "{\\q2}123456789", 2000, {100.0, 100.0});
	auto no_wrap_bounds = Bounds(no_wrap, FixedTextExtents);
	ASSERT_TRUE(no_wrap_bounds) << DescribeAssBoundsError(no_wrap_bounds.error);
	ExpectRect(no_wrap_bounds.value.rectangle, 0.0, 0.0, 90.0, 20.0);

	EvaluatedLine event_margins {MakeLine("ABCD"), {}};
	event_margins.line.Margin = {30, 30, 0};
	event_margins.state = EvaluateEffectiveAssState(
		{&file, &event_margins.line, {100.0, 100.0}, 2000});
	ASSERT_TRUE(event_margins.state);
	EXPECT_DOUBLE_EQ(
		40.0, event_margins.state.value.text_style.available_wrap_width);
	EXPECT_EQ(AssBoundsError::UnsupportedAutomaticWrap,
		Bounds(event_margins, FixedTextExtents).error);
}

TEST(perspective_ass_bounds, has_deterministic_fallback_and_rejects_invalid_text_metrics) {
	AssFile file;
	AddDefaultStyle(file);

	auto text = Evaluate(file, "AB");
	auto fallback = Bounds(text);
	ASSERT_TRUE(fallback) << DescribeAssBoundsError(fallback.error);
	ExpectRect(fallback.value.rectangle, 0.0, 0.0, 96.0, 48.0);

	auto unavailable = Bounds(text, FailedTextExtents);
	EXPECT_EQ(AssBoundsError::FontUnavailable, unavailable.error);
	EXPECT_EQ(BoundsKind::FontUnavailable, unavailable.value.kind);
	EXPECT_EQ("Arial", unavailable.font_name);

	auto invalid = Bounds(text, InvalidTextExtents);
	EXPECT_EQ(AssBoundsError::FontUnavailable, invalid.error);
	EXPECT_EQ(BoundsKind::FontUnavailable, invalid.value.kind);
	EXPECT_EQ("Arial", invalid.font_name);
	EXPECT_STRNE("unknown Perspective ASS bounds error",
		DescribeAssBoundsError(invalid.error));

	auto no_glyphs = Evaluate(file, "\\N");
	EXPECT_EQ(AssBoundsError::EmptyGeometry,
		Bounds(no_glyphs, FixedTextExtents).error);
}

TEST(perspective_ass_bounds, dynamic_geometry_keeps_read_only_drawing_bounds) {
	AssFile file;
	AddDefaultStyle(file);

	auto move = Evaluate(file,
		"{\\p1\\move(0,0,100,100)}m 0 0 l 20 0 20 10 0 10");
	ASSERT_TRUE(move.state) << DescribeAssStateError(move.state.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedMove, move.state.apply_blocker);
	ASSERT_TRUE(Bounds(move));

	auto animated = Evaluate(file,
		"{\\p1\\t(0,1000,\\frx20)}m 0 0 l 20 0 20 10 0 10", 1500);
	ASSERT_TRUE(animated.state) << DescribeAssStateError(animated.state.error);
	EXPECT_EQ(AssApplyBlocker::UnsupportedGeometryAnimation, animated.state.apply_blocker);
	ASSERT_TRUE(Bounds(animated));
}
