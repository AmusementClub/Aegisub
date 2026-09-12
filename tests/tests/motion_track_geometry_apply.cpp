#include <main.h>

#include "../../src/motion_track/apply_plan.h"
#include "../../src/perspective_ass_bounds.h"
#include "../../src/perspective_ass_state.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;
using namespace perspective;

struct Fixture {
	AssFile file;
	ApplyPlanInput input;
	AssDialogue *line;

	explicit Fixture(int frames = 10) {
		auto *style = new AssStyle;
		style->name = "Default";
		style->outline_w = 0;
		style->shadow_w = 0;
		file.Styles.push_back(*style);
		line = new AssDialogue;
		line->Start = 0;
		line->End = frames * 100;
		line->Text = R"({\an7\pos(300,200)\p1}m 0 0 l 120 0 120 40 0 40)";
		file.Events.push_back(*line);
		input.model = TrackModel::Affine;
		input.origin_center_x = 320;
		input.origin_center_y = 240;
		input.decode_interval = input.direction_domain = {.first = 0, .last = frames - 1};
		input.storage_width = input.script_width = 1920;
		input.storage_height = input.script_height = 1080;
		input.timecodes = agi::vfr::Framerate(10.0);
		input.video_frame_count = frames;
		input.options.mode = ApplyMode::Exact;
		input.options.compact_epsilon = 0.075;
		for (int frame = 0; frame < frames; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.model = input.model;
			sample.status = TrackStatus::Ok;
			sample.center_x = input.origin_center_x;
			sample.center_y = input.origin_center_y;
			input.samples.push_back(sample);
		}
	}

	void Matrix(int frame, std::array<double, 9> values) {
		auto& sample = input.samples.at(frame);
		sample.model = input.model;
		sample.transform.matrix = values;
		sample.center_x = input.origin_center_x + values[2] / values[8];
		sample.center_y = input.origin_center_y + values[5] / values[8];
	}

	[[nodiscard]] ForwardResult Render(AssDialogue const& event, int time) const {
		auto const evaluated = EvaluateEffectiveAssState({.file = &file, .line = &event, .play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)}, .capture_time_ms = time});
		EXPECT_TRUE(evaluated) << DescribeAssStateError(evaluated.error);
		if (!evaluated)
			return {.error = ForwardError::NonFiniteState};
		ForwardInput forward;
		forward.play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)};
		forward.video_storage_resolution = Resolution{.width = static_cast<double>(input.storage_width), .height = static_cast<double>(input.storage_height)};
		int const layout_x = file.GetScriptInfoAsInt("LayoutResX");
		int const layout_y = file.GetScriptInfoAsInt("LayoutResY");
		if (layout_x > 0 && layout_y > 0)
			forward.layout_resolution = Resolution{.width = static_cast<double>(layout_x), .height = static_cast<double>(layout_y)};
		auto const layout_aspect = ResolvePerspectiveLayoutAspect(forward);
		EXPECT_TRUE(layout_aspect.has_value());
		if (!layout_aspect)
			return {.error = ForwardError::InvalidResolution};
		auto const bounds = EvaluateAssBaseBounds({.line = &event, .state = &evaluated.value, .text_extents = input.text_extents, .layout_aspect = *layout_aspect});
		EXPECT_TRUE(bounds) << DescribeAssBoundsError(bounds.error);
		if (!bounds)
			return {.error = ForwardError::InvalidBounds};
		forward.bounds = bounds.value;
		forward.state = evaluated.value.transform;
		return ForwardQuad(forward);
	}

	[[nodiscard]] PlannedLinePart const *Part(MotionTrackApplyPlan const& plan, int frame) const {
		int const time = input.timecodes.TimeAtFrame(frame);
		for (auto const& planned : plan.lines) {
			if (planned.source != line)
				continue;
			for (auto const& part : planned.parts)
				if (part.covered && static_cast<int>(agi::Time(part.start_ms)) <= time && time < static_cast<int>(agi::Time(part.end_ms)))
					return &part;
		}
		return nullptr;
	}

	void ExpectFrames(MotionTrackApplyPlan const& plan, double tolerance) const {
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		ASSERT_TRUE(std::ranges::any_of(plan.lines, [&](auto const& planned) { return planned.source == line; }));
		auto const source = Render(*line, std::clamp(input.seed_time_ms,
													 line->Start.GetMillisecond(), line->End.GetMillisecond() - 1));
		ASSERT_TRUE(source) << DescribeForwardError(source.error);
		TrackSample const *previous = nullptr;
		for (auto const& sample : input.samples) {
			SCOPED_TRACE(sample.frame);
			if (sample.status == TrackStatus::Ok)
				previous = &sample;
			ASSERT_NE(nullptr, previous);
			int const time = input.timecodes.TimeAtFrame(sample.frame);
			if (time < static_cast<int>(line->Start) || time >= static_cast<int>(line->End))
				continue;
			auto const *part = Part(plan, sample.frame);
			ASSERT_NE(nullptr, part);
			AssDialogue event(*line);
			// The saved ASS event carries centiseconds. The evaluator accepts
			// raw milliseconds, so round here to verify actual serialized timing.
			event.Start = static_cast<int>(agi::Time(part->start_ms));
			event.End = static_cast<int>(agi::Time(part->end_ms));
			event.Text = part->text;
			auto const actual = Render(event, input.timecodes.TimeAtFrame(sample.frame));
			ASSERT_TRUE(actual) << DescribeForwardError(actual.error);
			double const x_ratio = static_cast<double>(input.storage_width) / input.script_width;
			double const y_ratio = static_cast<double>(input.storage_height) / input.script_height;
			for (size_t corner = 0; corner < source.quad.size(); ++corner) {
				double const x = source.quad[corner].x * x_ratio - input.origin_center_x;
				double const y = source.quad[corner].y * y_ratio - input.origin_center_y;
				auto const& h = previous->transform.matrix;
				double const w = h[6] * x + h[7] * y + h[8];
				double const expected_x = (h[0] * x + h[1] * y + h[2]) / w + input.origin_center_x;
				double const expected_y = (h[3] * x + h[4] * y + h[5]) / w + input.origin_center_y;
				double const error = std::hypot(actual.quad[corner].x * x_ratio - expected_x,
												actual.quad[corner].y * y_ratio - expected_y);
				EXPECT_LE(error, tolerance) << "corner " << corner << ": " << part->text;
			}
		}
	}
};

} // namespace

TEST(motion_track_geometry_apply, exact_affine_composes_existing_3d_shear_origin_and_nonuniform_resolutions) {
	Fixture fixture;
	fixture.input.script_width = 1280;
	fixture.input.script_height = 540;
	fixture.file.SetScriptInfo("PlayResX", "1280");
	fixture.file.SetScriptInfo("PlayResY", "540");
	fixture.file.SetScriptInfo("LayoutResX", "1024");
	fixture.file.SetScriptInfo("LayoutResY", "768");
	fixture.line->Text = R"({\an7\pos(300,200)\org(250,170)\frx12\fry-8\frz10\fax0.15\fay0.05\fscx110\fscy85\p1}m 0 0 l 120 0 120 40 0 40)";
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1 + 0.02 * frame, 0.015 * frame, 2.0 * frame,
							   -0.005 * frame, 1 - 0.01 * frame, -1.0 * frame, 0, 0, 1});
	auto const original = fixture.line->Text.get();
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, 0.05);
	EXPECT_EQ(original, fixture.line->Text.get());
	EXPECT_EQ(0, fixture.line->Start);
	EXPECT_EQ(1000, fixture.line->End);
}

TEST(motion_track_geometry_apply, exact_homography_matches_independent_four_corner_projection) {
	Fixture fixture;
	fixture.input.model = TrackModel::Homography;
	fixture.input.script_width = 960;
	fixture.file.SetScriptInfo("PlayResX", "960");
	fixture.file.SetScriptInfo("PlayResY", "1080");
	fixture.file.SetScriptInfo("LayoutResX", "1280");
	fixture.file.SetScriptInfo("LayoutResY", "720");
	fixture.line->Text = R"({\an7\pos(200,150)\frz13\p1}m 0 0 l 120 0 120 40 0 40)";
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1 + 0.004 * frame, 0.003 * frame, 1.5 * frame,
							   0.002 * frame, 1 - 0.003 * frame, -0.5 * frame,
							   0.00008 * frame, -0.00004 * frame, 1});
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, 0.05);
	EXPECT_EQ(10u, plan.event_count);
}

TEST(motion_track_geometry_apply, exact_position_precision_applies_even_to_unchanged_geometry) {
	for (auto model : {TrackModel::Translation, TrackModel::Similarity,
					   TrackModel::Affine, TrackModel::Homography}) {
		SCOPED_TRACE(static_cast<int>(model));
		Fixture fixture(3);
		fixture.input.model = model;
		fixture.line->Text = R"({\an7\pos(300.031,200.019)\org(250,170)\p1}m 0 0 l 120 0 120 40 0 40)";
		auto const original = fixture.line->Text.get();
		fixture.input.options.position_decimals = 0;
		auto const coarse = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
		fixture.ExpectFrames(coarse, 0.05);
		ASSERT_TRUE(coarse.has_mutations()) << coarse.message;
		EXPECT_NE(std::string::npos, coarse.lines.front().parts.front().text.find(R"(\pos(300,200))"));
		EXPECT_DOUBLE_EQ(300.0, coarse.lines.front().parts.front().x0);
		EXPECT_DOUBLE_EQ(200.0, coarse.lines.front().parts.front().y0);

		fixture.input.options.position_decimals = 2;
		auto const precise = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
		fixture.ExpectFrames(precise, 0.05);
		ASSERT_TRUE(precise.has_mutations()) << precise.message;
		EXPECT_NE(std::string::npos, precise.lines.front().parts.front().text.find(R"(\pos(300.03,200.02))"));
		EXPECT_NE(coarse.lines.front().parts.front().text, precise.lines.front().parts.front().text);
		EXPECT_DOUBLE_EQ(300.03, precise.lines.front().parts.front().x0);
		EXPECT_DOUBLE_EQ(200.02, precise.lines.front().parts.front().y0);
		EXPECT_EQ(original, fixture.line->Text.get());
	}
}

TEST(motion_track_geometry_apply, compact_position_quantization_uses_output_pixel_budget_and_move_precision) {
	Fixture fixture(4);
	fixture.input.options.mode = ApplyMode::Compact;
	fixture.input.options.compact_epsilon = 0.2;
	fixture.input.options.position_decimals = 1;
	fixture.input.script_width = 960;
	fixture.input.script_height = 360;
	fixture.line->Text = R"({\an7\pos(300.04,200.04)\p1}m 0 0 l 120 0 120 40 0 40)";
	for (int frame = 0; frame < 4; ++frame)
		fixture.Matrix(frame, {1, 0, 20.0 * frame, 0, 1, 0, 0, 0, 1});
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	// The 0.04 script-pixel rounding adds sqrt(0.08^2 + 0.12^2) video
	// pixels: above the solver's internal 0.05 threshold, below the user's 0.2.
	fixture.ExpectFrames(plan, 0.2);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_EQ(1u, plan.event_count);
	auto const& part = plan.lines.front().parts.front();
	EXPECT_NE(std::string::npos, part.text.find(R"(\move(300,200,330,200,0,300))"));
	EXPECT_DOUBLE_EQ(300.0, part.x0);
	EXPECT_DOUBLE_EQ(330.0, part.x1);
}

TEST(motion_track_geometry_apply, insufficient_position_precision_rejects_all_targets_and_recovers) {
	Fixture fixture(3);
	fixture.input.options.position_decimals = 0;
	auto *second = new AssDialogue(*fixture.line);
	second->Text = R"({\an7\pos(300.4,200)\org(250,170)\frx12\fry-8\p1}m 0 0 l 120 0 120 40 0 40)";
	fixture.file.Events.push_back(*second);
	auto const first_original = fixture.line->Text.get();
	auto const second_original = second->Text.get();
	auto const first_only = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	ASSERT_TRUE(first_only.has_mutations()) << first_only.message;
	auto const rejected = BuildApplyPlan(fixture.file, {fixture.line, second}, fixture.input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, rejected.status);
	EXPECT_FALSE(rejected.has_mutations());
	EXPECT_TRUE(rejected.lines.empty());
	EXPECT_EQ(0u, rejected.event_count);
	EXPECT_NE(std::string::npos, rejected.message.find("Position decimals"));
	EXPECT_EQ(first_original, fixture.line->Text.get());
	EXPECT_EQ(second_original, second->Text.get());

	fixture.input.options.position_decimals = 3;
	auto const accepted = BuildApplyPlan(fixture.file, {fixture.line, second}, fixture.input);
	ASSERT_TRUE(accepted.has_mutations()) << accepted.message;
	ASSERT_EQ(2u, accepted.lines.size());
	fixture.ExpectFrames(accepted, 0.05);
	fixture.line = second;
	fixture.ExpectFrames(accepted, 0.05);
}

TEST(motion_track_geometry_apply, text_homography_calibrates_layout_glyph_width_and_spacing_before_projection) {
	Fixture fixture;
	fixture.input.model = TrackModel::Homography;
	fixture.input.script_width = 960;
	fixture.input.script_height = 540;
	fixture.file.SetScriptInfo("PlayResX", "960");
	fixture.file.SetScriptInfo("PlayResY", "540");
	fixture.file.SetScriptInfo("LayoutResX", "1920");
	fixture.file.SetScriptInfo("LayoutResY", "540");
	fixture.line->Text = R"({\an6\pos(400,240)\org(360,220)\frz-17\frx8\fry-6\fscx115\fscy90\fs40\fsp3}TRACK)";
	fixture.input.text_extents = [](AssStyle *style, std::string const& text,
									double& width, double& height, double& descent, double& leading) {
		EXPECT_EQ("TRACK", text);
		EXPECT_DOUBLE_EQ(40.0, style->fontsize);
		EXPECT_DOUBLE_EQ(100.0, style->scalex);
		EXPECT_DOUBLE_EQ(100.0, style->scaley);
		// Layout has twice as many horizontal pixels per script unit:
		// spacing follows that ratio, while the glyph measurement follows Y.
		EXPECT_DOUBLE_EQ(6.0, style->spacing);
		width = 60.0 + 4.0 * style->spacing;
		height = 24.0;
		descent = 4.0;
		leading = 0.0;
		return true;
	};
	auto const evaluated = EvaluateEffectiveAssState({.file = &fixture.file, .line = fixture.line, .play_resolution = {.width = 960, .height = 540}, .capture_time_ms = 0});
	ASSERT_TRUE(evaluated) << DescribeAssStateError(evaluated.error);
	ASSERT_FALSE(evaluated.value.drawing_mode);
	EXPECT_EQ(6, evaluated.value.transform.alignment);
	EXPECT_DOUBLE_EQ(3.0, evaluated.value.text_style.spacing);
	ForwardInput metric;
	metric.play_resolution = {.width = 960, .height = 540};
	metric.layout_resolution = Resolution{.width = 1920, .height = 540};
	metric.video_storage_resolution = Resolution{.width = 1920, .height = 1080};
	auto const aspect = ResolvePerspectiveLayoutAspect(metric);
	ASSERT_TRUE(aspect.has_value());
	EXPECT_DOUBLE_EQ(2.0, *aspect);
	auto const bounds = EvaluateAssBaseBounds({.line = fixture.line, .state = &evaluated.value, .text_extents = fixture.input.text_extents, .layout_aspect = *aspect});
	ASSERT_TRUE(bounds) << DescribeAssBoundsError(bounds.error);
	EXPECT_EQ(BoundsKind::Text, bounds.value.kind);
	EXPECT_DOUBLE_EQ(0.0, bounds.value.rectangle.left);
	EXPECT_DOUBLE_EQ(0.0, bounds.value.rectangle.top);
	EXPECT_DOUBLE_EQ(42.0, bounds.value.rectangle.right); // 60 / 2 glyph width + 4 * 3 spacing
	EXPECT_DOUBLE_EQ(24.0, bounds.value.rectangle.bottom);
	auto const shift = ResolveBoundsAlignmentShift(bounds.value, 6);
	EXPECT_DOUBLE_EQ(-42.0, shift.x);
	EXPECT_DOUBLE_EQ(-12.0, shift.y);

	// A second ratio checks the callback's input as well as the returned box;
	// otherwise production and verification could both accidentally use 1.
	auto const half_aspect_measurer = [](AssStyle *style, std::string const& text,
										 double& width, double& height, double& descent, double& leading) {
		EXPECT_EQ("TRACK", text);
		EXPECT_DOUBLE_EQ(1.5, style->spacing);
		width = 60.0 + 4.0 * style->spacing;
		height = 24.0;
		descent = 4.0;
		leading = 0.0;
		return true;
	};
	auto const narrower_layout = EvaluateAssBaseBounds({.line = fixture.line, .state = &evaluated.value, .text_extents = half_aspect_measurer, .layout_aspect = 0.5});
	ASSERT_TRUE(narrower_layout) << DescribeAssBoundsError(narrower_layout.error);
	EXPECT_DOUBLE_EQ(132.0, narrower_layout.value.rectangle.right); // 60 / 0.5 + 4 * 3
	EXPECT_DOUBLE_EQ(24.0, narrower_layout.value.rectangle.bottom);
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1 + 0.01 * frame, 0.005 * frame, 2.0 * frame,
							   -0.003 * frame, 1 - 0.005 * frame, -1.0 * frame,
							   0.00008 * frame, -0.00004 * frame, 1});
	auto const original = fixture.line->Text.get();
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, 0.05);
	EXPECT_EQ(original, fixture.line->Text.get());
}

TEST(motion_track_geometry_apply, vertical_font_affine_tracking_preserves_semantic_z_rotation) {
	Fixture fixture(6);
	fixture.line->Text = R"({\fn@Vertical Motion\an5\pos(300,200)\frz90\fs32}VERT)";
	fixture.input.text_extents = [](AssStyle *style, std::string const& text,
									double& width, double& height, double& descent, double& leading) {
		EXPECT_EQ("@Vertical Motion", style->font);
		EXPECT_EQ("VERT", text);
		EXPECT_DOUBLE_EQ(32.0, style->fontsize);
		EXPECT_DOUBLE_EQ(100.0, style->scalex);
		EXPECT_DOUBLE_EQ(100.0, style->scaley);
		width = 48.0;
		height = 18.0;
		descent = 3.0;
		leading = 0.0;
		return true;
	};
	for (int frame = 0; frame < 6; ++frame)
		fixture.Matrix(frame, {1 + 0.01 * frame, 0.02 * frame, 2.0 * frame,
							   -0.005 * frame, 1 - 0.005 * frame, -1.0 * frame, 0, 0, 1});
	auto const original = fixture.line->Text.get();
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, 0.05);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	for (int frame = 0; frame < 6; ++frame) {
		SCOPED_TRACE(frame);
		auto const *part = fixture.Part(plan, frame);
		ASSERT_NE(nullptr, part);
		EXPECT_NE(std::string::npos, part->text.find(R"(\fn@Vertical Motion\)"));
		EXPECT_TRUE(part->text.ends_with("VERT"));
		AssDialogue emitted(*fixture.line);
		emitted.Start = static_cast<int>(agi::Time(part->start_ms));
		emitted.End = static_cast<int>(agi::Time(part->end_ms));
		emitted.Text = part->text;
		auto const evaluated = EvaluateEffectiveAssState({.file = &fixture.file, .line = &emitted, .play_resolution = {.width = 1920, .height = 1080}, .capture_time_ms = fixture.input.timecodes.TimeAtFrame(frame)});
		ASSERT_TRUE(evaluated) << DescribeAssStateError(evaluated.error);
		EXPECT_DOUBLE_EQ(90.0, evaluated.value.transform.rotation_z);
		EXPECT_EQ("@Vertical Motion", evaluated.value.text_style.font_name);
	}
	EXPECT_EQ(original, fixture.line->Text.get());
}

TEST(motion_track_geometry_apply, translation_moves_clip_and_original_rotation_origin_with_glyphs) {
	Fixture fixture;
	fixture.input.model = TrackModel::Translation;
	fixture.input.options.mode = ApplyMode::Compact;
	fixture.line->Text = R"({\an7\pos(300,200)\org(260,170)\frz20\clip(200,100,500,350)\bord0\p1}m 0 0 l 120 0 120 40 0 40)";
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1, 0, 2.0 * frame, 0, 1, -1.0 * frame, 0, 0, 1});
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, 0.05);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	EXPECT_TRUE(plan.needs_manual_review.empty());
	for (int frame = 0; frame < 10; ++frame) {
		auto const *part = fixture.Part(plan, frame);
		ASSERT_NE(nullptr, part);
		std::string const expected = "\\clip(" + std::to_string(200 + 2 * frame) + "," + std::to_string(100 - frame) + "," + std::to_string(500 + 2 * frame) + "," + std::to_string(350 - frame) + ")";
		EXPECT_NE(std::string::npos, part->text.find(expected));
	}
}

TEST(motion_track_geometry_apply, compact_linear_nonuniform_affine_scale_fits_one_event) {
	Fixture fixture(20);
	fixture.input.options.mode = ApplyMode::Compact;
	for (int frame = 0; frame < 20; ++frame)
		fixture.Matrix(frame, {1 + 0.01 * frame, 0, 0, 0, 1 + 0.005 * frame, 0, 0, 0, 1});
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, fixture.input.options.compact_epsilon);
	ASSERT_EQ(1u, plan.event_count) << plan.message;
	auto const *part = fixture.Part(plan, 10);
	ASSERT_NE(nullptr, part);
	EXPECT_NE(std::string::npos, part->text.find("\\t("));
}

TEST(motion_track_geometry_apply, compact_nonlinear_projective_motion_respects_every_frame_error_budget) {
	Fixture fixture(20);
	fixture.input.model = TrackModel::Homography;
	fixture.input.options.mode = ApplyMode::Compact;
	fixture.input.options.compact_epsilon = 0.05;
	for (int frame = 0; frame < 20; ++frame) {
		double const wave = std::sin(frame * 0.4);
		fixture.Matrix(frame, {1 + 0.1 * wave, 0.03 * wave, 3 * wave,
							   0.01 * wave, 1 - 0.07 * wave, -2 * wave, 0.0008 * wave, -0.0004 * wave, 1});
	}
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, fixture.input.options.compact_epsilon);
	EXPECT_GT(plan.event_count, 1u);
	EXPECT_LE(plan.event_count, 20u);
}

TEST(motion_track_geometry_apply, failed_gap_holds_full_affine_pose_and_resumes_at_next_good_frame) {
	Fixture fixture;
	fixture.input.options.mode = ApplyMode::Compact;
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1 + 0.025 * frame, 0.02 * frame, 2.0 * frame,
							   0, 1 - 0.01 * frame, 0, 0, 0, 1});
	for (int frame = 3; frame <= 5; ++frame) {
		fixture.input.samples[frame].status = TrackStatus::Failed;
		fixture.input.samples[frame].transform.matrix.fill(1000);
	}
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, fixture.input.options.compact_epsilon);
	auto const *first_held = fixture.Part(plan, 3);
	auto const *last_held = fixture.Part(plan, 5);
	ASSERT_NE(nullptr, first_held);
	ASSERT_NE(nullptr, last_held);
	EXPECT_EQ(first_held->text, last_held->text);
	EXPECT_EQ(std::string::npos, first_held->text.find("\\t("));
	EXPECT_EQ(std::string::npos, first_held->text.find("\\move("));
}

TEST(motion_track_geometry_apply, invalid_projective_frame_rejects_the_whole_plan_without_mutation) {
	Fixture fixture;
	fixture.input.model = TrackModel::Homography;
	fixture.Matrix(9, {1, 0, 0, 0, 1, 0, 0, 0, 0});
	fixture.input.samples[9].center_x = fixture.input.origin_center_x;
	fixture.input.samples[9].center_y = fixture.input.origin_center_y;
	auto const original = fixture.line->Text.get();
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, plan.status);
	EXPECT_FALSE(plan.has_mutations());
	EXPECT_TRUE(plan.lines.empty());
	EXPECT_EQ(0u, plan.event_count);
	EXPECT_FALSE(plan.message.empty());
	EXPECT_EQ(original, fixture.line->Text.get());
	EXPECT_EQ(0, fixture.line->Start);
	EXPECT_EQ(1000, fixture.line->End);
}

TEST(motion_track_geometry_apply, animated_clip_rejection_is_atomic_across_selected_lines) {
	Fixture fixture;
	fixture.line->Text = R"({\an7\pos(300,200)\clip(0,0,600,400)\t(0,800,\clip(20,10,500,300))\p1}m 0 0 l 120 0 120 40 0 40)";
	auto *other = new AssDialogue(*fixture.line);
	other->Text = R"({\an7\pos(100,100)\p1}m 0 0 l 30 0 30 20 0 20)";
	fixture.file.Events.push_back(*other);
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1, 0.02 * frame, 1.0 * frame, 0, 1, 0, 0, 0, 1});
	auto const original = fixture.line->Text.get(), other_original = other->Text.get();
	auto const plan = BuildApplyPlan(fixture.file, {other, fixture.line}, fixture.input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, plan.status);
	EXPECT_FALSE(plan.has_mutations());
	EXPECT_TRUE(plan.lines.empty());
	EXPECT_EQ(0u, plan.event_count);
	EXPECT_FALSE(plan.message.empty());
	EXPECT_EQ(original, fixture.line->Text.get());
	EXPECT_EQ(other_original, other->Text.get());
}

TEST(motion_track_geometry_apply, selected_lines_outside_seed_time_retain_independent_geometry) {
	Fixture fixture;
	auto *later = new AssDialogue(*fixture.line);
	later->Start = 403;
	later->Text = R"({\an7\pos(100,120)\org(90,110)\frz15\p1}m 0 0 l 40 0 40 20 0 20)";
	fixture.file.Events.push_back(*later);
	for (int frame = 0; frame < 10; ++frame)
		fixture.Matrix(frame, {1 + 0.02 * frame, 0.01 * frame, 2.0 * frame,
							   0, 1 - 0.01 * frame, 0, 0, 0, 1});
	auto const original = fixture.line->Text.get();
	auto const later_original = later->Text.get();
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line, later}, fixture.input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(2u, plan.lines.size());
	fixture.ExpectFrames(plan, 0.05);
	EXPECT_EQ(original, fixture.line->Text.get());
	fixture.line = later;
	fixture.ExpectFrames(plan, 0.05);
	EXPECT_EQ(later_original, later->Text.get());
	EXPECT_EQ(400, later->Start);
	EXPECT_EQ(403, later->Start.GetMillisecond());
	EXPECT_EQ(1000, later->End);
}

TEST(motion_track_geometry_apply, vfr_compact_uses_serialized_event_start_after_held_gap) {
	Fixture fixture(20);
	std::vector<int> const times{0, 41, 88, 127, 173, 217, 263, 310, 349, 394,
								 441, 489, 535, 578, 619, 667, 712, 759, 805, 853};
	fixture.input.timecodes = agi::vfr::Framerate(times);
	fixture.line->End = 900;
	fixture.input.options.mode = ApplyMode::Compact;
	fixture.input.options.compact_epsilon = 0.025;
	for (int frame = 0; frame < 20; ++frame) {
		double const time = times[frame];
		fixture.Matrix(frame, {1 + 0.0008 * time, 0, 0.1 * time,
							   0, 1 + 0.0004 * time, -0.05 * time, 0, 0, 1});
	}
	fixture.input.samples[4].status = TrackStatus::Failed;
	auto const plan = BuildApplyPlan(fixture.file, {fixture.line}, fixture.input);
	fixture.ExpectFrames(plan, fixture.input.options.compact_epsilon);
	ASSERT_EQ(3u, plan.event_count) << plan.message;
	auto const *resumed = fixture.Part(plan, 5);
	ASSERT_NE(nullptr, resumed);
	EXPECT_EQ(195, resumed->start_ms);
	EXPECT_EQ(200, static_cast<int>(agi::Time(resumed->start_ms)));
	EXPECT_NE(std::string::npos, resumed->text.find("\\t("));
}
