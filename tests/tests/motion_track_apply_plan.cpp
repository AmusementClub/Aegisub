#include <main.h>

#include "../../src/motion_track/apply_plan.h"
#include "../../src/motion_track/types.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;

struct Fixture {
	AssFile file;
	agi::vfr::Framerate fps = agi::vfr::Framerate(10.0); // 100 ms frames
	std::vector<AssDialogue*> lines;

	Fixture() {
		auto* style = new AssStyle;
		style->name = "Default";
		file.Styles.push_back(*style);
	}

	AssDialogue* AddLine(int start_ms, int end_ms, std::string text,
	                    bool comment = false) {
		auto* line = new AssDialogue;
		line->Start = start_ms;
		line->End = end_ms;
		line->Text = std::move(text);
		line->Comment = comment;
		file.Events.push_back(*line);
		lines.push_back(line);
		return line;
	}
};

ApplyPlanInput BaseInput() {
	ApplyPlanInput in;
	in.model = TrackModel::Translation;
	in.decode_interval = FrameInterval{0, 29};
	in.direction_domain = FrameInterval{0, 29};
	in.storage_width = 1920;
	in.storage_height = 1080;
	in.script_width = 1920;
	in.script_height = 1080;
	in.timecodes = agi::vfr::Framerate(10.0);
	in.video_frame_count = 30;
	in.seed_time_ms = 0;
	return in;
}

// Trajectory: origin_center (0,0); Ok sample per frame at center
// (cx0 + vx*f, cy0 + vy*f); frames outside [ok_first, ok_last] are absent.
std::vector<TrackSample> LinearSamples(int ok_first, int ok_last, double cx0,
                                       double cy0, double vx, double vy) {
	std::vector<TrackSample> samples;
	for (int f = ok_first; f <= ok_last; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		s.center_x = cx0 + vx * f;
		s.center_y = cy0 + vy * f;
		samples.push_back(s);
	}
	return samples;
}

void MarkFailed(std::vector<TrackSample>& samples, int first, int last) {
	for (auto& s : samples)
		if (s.frame >= first && s.frame <= last) s.status = TrackStatus::Failed;
}

std::string PartSummary(PlannedLinePart const& p) {
	return "{" + std::to_string(p.start_ms) + "-" +
	       std::to_string(p.end_ms) + (p.covered ? "+" : "=") + "}" + p.text;
}

struct TagPos {
	bool is_move = false;
	double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
	int t1 = 0, t2 = 0;
};

bool ParseTag(std::string const& text, TagPos& out) {
	auto next_comma = [&](std::string const& s, size_t& i) -> bool {
		if (i >= s.size() || s[i] != ',') return false;
		++i;
		return true;
	};
	auto parse_double = [&](std::string const& s, size_t& i, double& v)
	    -> bool {
		char* end = nullptr;
		v = std::strtod(s.c_str() + i, &end);
		if (end == s.c_str() + i) return false;
		i = size_t(end - s.c_str());
		return true;
	};
	auto parse_int = [&](std::string const& s, size_t& i, int& v) -> bool {
		char* end = nullptr;
		v = int(std::strtol(s.c_str() + i, &end, 10));
		if (end == s.c_str() + i) return false;
		i = size_t(end - s.c_str());
		return true;
	};

	size_t at = text.find("\\move(");
	if (at != std::string::npos) {
		size_t i = at + 6;
		out.is_move = true;
		return parse_double(text, i, out.x1) && next_comma(text, i)
		    && parse_double(text, i, out.y1) && next_comma(text, i)
		    && parse_double(text, i, out.x2) && next_comma(text, i)
		    && parse_double(text, i, out.y2) && next_comma(text, i)
		    && parse_int(text, i, out.t1) && next_comma(text, i)
		    && parse_int(text, i, out.t2);
	}
	at = text.find("\\pos(");
	if (at != std::string::npos) {
		size_t i = at + 5;
		out.is_move = false;
		out.t1 = out.t2 = 0;
		return parse_double(text, i, out.x1) && next_comma(text, i)
		    && parse_double(text, i, out.y1)
		    && (out.x2 = out.x1, out.y2 = out.y1, true);
	}
	return false;
}

// Position a renderer would show at frame f: the part whose [start,end) ms
// window contains the frame's exact timecode, with \move clamped/interpolated
// by its explicit t1/t2 window.
bool PositionAtFrame(std::vector<PlannedLinePart> const& parts,
                     agi::vfr::Framerate const& fps, int frame,
                     double& x, double& y) {
	int const tc = fps.TimeAtFrame(frame);
	for (auto const& part : parts) {
		if (!part.covered) continue;
		if (tc < part.start_ms || tc >= part.end_ms) continue;
		TagPos tag;
		if (!ParseTag(part.text, tag)) return false;
		if (!tag.is_move || tc - part.start_ms <= tag.t1) {
			x = tag.x1;
			y = tag.y1;
		} else if (tc - part.start_ms >= tag.t2) {
			x = tag.x2;
			y = tag.y2;
		} else {
			double const t =
			    double(tc - part.start_ms - tag.t1) / double(tag.t2 - tag.t1);
			x = tag.x1 + (tag.x2 - tag.x1) * t;
			y = tag.y1 + (tag.y2 - tag.y1) * t;
		}
		return true;
	}
	return false;
}
}

TEST(motion_track_apply_plan, constant_delta_emits_single_pos_part) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(100,200)}hello)");
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 100, 200, 0, 0);
	input.origin_center_x = 100;
	input.origin_center_y = 200;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	EXPECT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(1u, plan.lines.size());
	// vfr END lands mid-frame at this rate, so a small preserved suffix
	// (original text) trails the covered span.
	ASSERT_FALSE(plan.lines[0].parts.empty());
	auto const& covered = plan.lines[0].parts.front();
	EXPECT_TRUE(covered.covered);
	EXPECT_NE(std::string::npos,
	          covered.text.find(R"(\pos(100.00,200.00))"));
	EXPECT_EQ(1u, plan.event_count);
	EXPECT_EQ(1u, plan.event_count);
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts[0].text.find(R"(\pos(100.00,200.00))"));
	EXPECT_EQ(std::string::npos,
	          plan.lines[0].parts[0].text.find(R"(\pos(100,200))"));
	EXPECT_NE(std::string::npos, plan.lines[0].parts[0].text.find("hello"));
}

TEST(motion_track_apply_plan, linear_motion_compact_single_move_segment) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(50,60)}slide)");
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 0, 0, 2.0, 0.0); // delta = 2 px/frame
	auto opt = ApplyPlanOptions{};
	opt.compact_epsilon = 100.0; // whole domain collapses to one segment
	input.options = opt;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	// Line spans frames 0..19 at 10 fps; last position = 50+2*19 = 88.
	// The window is explicit so the endpoints land on the frames they were
	// sampled at: the part runs 0..1950ms (vfr END rounding), frame 0 renders
	// at 0ms -- nudged to 1 because <= 0 reads as "no explicit window" -- and
	// frame 19 renders at 1900ms.
	bool found_move = false;
	for (auto const& part : plan.lines[0].parts)
		found_move = found_move
		          || part.text.find(R"(\move(50.00,60.00,88.00,60.00,1,1900))")
		                 != std::string::npos;
	EXPECT_TRUE(found_move) << PartSummary(plan.lines[0].parts.front());
}

TEST(motion_track_apply_plan, covered_part_geometry_matches_emitted_tags) {
	Fixture fx;
	auto* line = fx.AddLine(0, 3000, "hello");
	auto input = BaseInput();
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	// Curved (quadratic) path: compact fitting needs several move segments.
	std::vector<TrackSample> samples;
	for (int f = 0; f <= 29; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		s.center_x = 100.0 + 3.0 * f;
		s.center_y = 100.0 + 0.08 * f * f;
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	size_t covered_count = 0;
	for (auto const& part : plan.lines[0].parts) {
		if (!part.covered) continue; // vfr END rounding leaves a tail
		++covered_count;
		TagPos tag;
		ASSERT_TRUE(ParseTag(part.text, tag));
		// The geometry fields carry the exact fitted positions; the tag text
		// rounds them to position_decimals, so compare within that rounding.
		double const tol = 0.5 * std::pow(10.0, -input.options.position_decimals)
		                 + 1e-9;
		EXPECT_NEAR(tag.x1, part.x0, tol);
		EXPECT_NEAR(tag.y1, part.y0, tol);
		if (tag.is_move) {
			EXPECT_NEAR(tag.x2, part.x1, tol);
			EXPECT_NEAR(tag.y2, part.y1, tol);
		} else {
			EXPECT_DOUBLE_EQ(part.x0, part.x1);
			EXPECT_DOUBLE_EQ(part.y0, part.y1);
		}
	}
	// The curvature must have forced more than one move segment.
	EXPECT_GE(covered_count, 2u);
}

TEST(motion_track_apply_plan, interior_failed_gap_holds_previous_ok_position) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(0,0)}track)");
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 10, 20, 3.0, 0.0);
	MarkFailed(input.samples, 5, 8);
	auto opt = ApplyPlanOptions{};
	opt.compact_epsilon = 0.01; // force per-frame pieces elsewhere
	input.options = opt;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());

	// The gap frames 5..8 must be covered by a part holding frame 4's center
	// (10+3*4=22,20), with hard cut points so nothing interpolates across.
	int const gap_start = input.timecodes.TimeAtFrame(5, agi::vfr::Time::START);
	int const gap_end = input.timecodes.TimeAtFrame(9, agi::vfr::Time::START);
	bool found_hold = false;
	for (auto const& part : plan.lines[0].parts) {
		if (part.start_ms == gap_start && part.end_ms == gap_end) {
			found_hold = true;
			EXPECT_NE(std::string::npos,
			          part.text.find(R"(\pos(22.00,20.00))"))
			    << PartSummary(part);
		}
	}
	EXPECT_TRUE(found_hold);

	// Timeline tiles without overlap or gaps.
	int prev_end = 0;
	for (auto const& part : plan.lines[0].parts) {
		EXPECT_EQ(prev_end, part.start_ms);
		prev_end = part.end_ms;
	}
	EXPECT_EQ(2000, prev_end);
}

TEST(motion_track_apply_plan, leading_and_trailing_gaps_block_apply) {
	{
		Fixture fx;
		auto* line = fx.AddLine(0, 2000, "hi");
		auto input = BaseInput();
		input.samples = LinearSamples(4, 29, 0, 0, 1.0, 0.0); // leading Missing
		auto plan = BuildApplyPlan(fx.file, {line}, input);
		EXPECT_EQ(ApplyPlanStatus::IncompleteCoverage, plan.status);
		EXPECT_TRUE(plan.lines.empty());
		ASSERT_EQ(1u, plan.uncovered.size());
		ASSERT_FALSE(plan.uncovered[0].ranges.empty());
		EXPECT_EQ(0, plan.uncovered[0].ranges[0].first) << "got " << plan.uncovered[0].ranges[0].first;
		EXPECT_EQ(3, plan.uncovered[0].ranges[0].last) << "got " << plan.uncovered[0].ranges[0].last;
	}
	{
		Fixture fx;
		auto* line = fx.AddLine(0, 3500, "hi"); // domain reaches frame 29
		auto input = BaseInput();
		input.samples = LinearSamples(0, 25, 0, 0, 1.0, 0.0); // frames 26..29 absent
		auto plan = BuildApplyPlan(fx.file, {line}, input);
		EXPECT_EQ(ApplyPlanStatus::IncompleteCoverage, plan.status);
		ASSERT_EQ(1u, plan.uncovered.size());
		ASSERT_FALSE(plan.uncovered[0].ranges.empty());
		EXPECT_EQ(26, plan.uncovered[0].ranges.back().first);
		EXPECT_EQ(29, plan.uncovered[0].ranges.back().last);
	}
}

TEST(motion_track_apply_plan, exact_mode_groups_identical_rounded_positions) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(0,0)}x)");
	auto input = BaseInput();
	input.options.mode = ApplyMode::Exact;
	// Positions: frames 0..9 -> (10,0); 10..14 -> (12,0); 15..29 -> (10,0).
	std::vector<TrackSample> samples;
	for (int f = 0; f <= 29; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		double dx = f < 10 ? 10.0 : (f < 15 ? 12.0 : 10.0);
		s.center_x = dx;
		s.center_y = 0;
		samples.push_back(s);
	}
	input.samples = std::move(samples);

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	EXPECT_EQ(3u, plan.event_count);
	// 3 covered \pos groups + the vfr-END trailing sliver (original text).
	ASSERT_EQ(4u, plan.lines[0].parts.size());

	for (size_t i = 0; i < 3; ++i) {
		EXPECT_TRUE(plan.lines[0].parts[i].covered);
		EXPECT_EQ(std::string::npos,
		          plan.lines[0].parts[i].text.find("\\move"));
		EXPECT_NE(std::string::npos,
		          plan.lines[0].parts[i].text.find("\\pos("));
	}
	EXPECT_FALSE(plan.lines[0].parts[3].covered);
	EXPECT_EQ(line->Text, plan.lines[0].parts[3].text);
	int const split_a = input.timecodes.TimeAtFrame(10, agi::vfr::Time::START);
	int const split_b = input.timecodes.TimeAtFrame(15, agi::vfr::Time::START);
	EXPECT_EQ(split_a, plan.lines[0].parts[0].end_ms);
	EXPECT_EQ(split_a, plan.lines[0].parts[1].start_ms);
	EXPECT_EQ(split_b, plan.lines[0].parts[1].end_ms);
	EXPECT_EQ(split_b, plan.lines[0].parts[2].start_ms);
}

TEST(motion_track_apply_plan, comment_lines_are_skipped_silently) {
	Fixture fx;
	auto* comment = fx.AddLine(0, 2000, "commentary", /*comment=*/true);
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 0, 0, 0.0, 0.0);

	auto plan = BuildApplyPlan(fx.file, {comment}, input);
	EXPECT_EQ(ApplyPlanStatus::Ok, plan.status);
	EXPECT_TRUE(plan.lines.empty());
	EXPECT_EQ(0u, plan.event_count);
}

TEST(motion_track_apply_plan, shorter_line_uses_only_its_own_segment) {
	// A trajectory covering frames 0..29 applied to a line that only spans
	// frames 10..29: the line consumes just its own slice of the trajectory,
	// anchored at its own frame-10 position.
	Fixture fx;
	auto* full = fx.AddLine(0, 3000, R"({\pos(0,0)}full)");
	auto* part = fx.AddLine(1000, 3000, R"({\pos(0,0)}part)");
	auto input = BaseInput();
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 0;
	input.origin_center_y = 0;
	input.samples = LinearSamples(0, 29, 0, 0, 3.0, 0.0);

	auto plan = BuildApplyPlan(fx.file, {full, part}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(2u, plan.lines.size());
	ASSERT_EQ(full, plan.lines[0].source);
	ASSERT_EQ(part, plan.lines[1].source);
	// The full line starts at the trajectory origin, the short line starts at
	// its own frame-10 position (3 px/frame * 10) — not at the origin.
	bool full_ok = false, part_ok = false;
	for (auto const& p : plan.lines[0].parts)
		full_ok = full_ok
		    || p.text.find(R"(\pos(0.00,0.00))") != std::string::npos;
	for (auto const& p : plan.lines[1].parts)
		part_ok = part_ok
		    || p.text.find(R"(\pos(30.00,0.00))") != std::string::npos;
	EXPECT_TRUE(full_ok);
	EXPECT_TRUE(part_ok);
	// Nothing from before frame 10 leaks into the short line.
	for (auto const& p : plan.lines[1].parts) {
		if (!p.covered) continue;
		TagPos tag;
		ASSERT_TRUE(ParseTag(p.text, tag));
		EXPECT_GE(tag.x1, 30.0 - 0.001);
	}
}

TEST(motion_track_apply_plan, out_of_domain_prefix_and_suffix_preserved) {
	Fixture fx;
	auto* line = fx.AddLine(500, 2500, R"({\pos(0,0)}keep me)");
	auto input = BaseInput();
	input.direction_domain = FrameInterval{8, 21};
	input.samples = LinearSamples(0, 29, 0, 0, 0.0, 0.0);
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(3u, parts.size());

	std::string const original = line->Text; // preserved verbatim, tags included
	EXPECT_FALSE(parts[0].covered);
	EXPECT_EQ(500, parts[0].start_ms);
	EXPECT_EQ(original, parts[0].text);
	EXPECT_TRUE(parts[1].covered);
	EXPECT_FALSE(parts[2].covered); // vfr-END trailing sliver stays original
	EXPECT_EQ(original, parts.back().text);
	EXPECT_EQ(2500, parts.back().end_ms);
	// Covered span tiles seamlessly against the preserved prefix.
	EXPECT_EQ(parts[0].end_ms, parts[1].start_ms);
}

TEST(motion_track_apply_plan, exact_over_hundred_events_needs_confirmation) {
	Fixture fx;
	auto* line = fx.AddLine(0, 12500, "x");
	auto input = BaseInput();
	input.video_frame_count = 130;
	input.decode_interval = FrameInterval{0, 129};
	input.direction_domain = FrameInterval{0, 129};
	input.options.mode = ApplyMode::Exact;
	std::vector<TrackSample> samples;
	for (int f = 0; f <= 129; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		s.center_x = f % 2; // alternates -> one group per frame
		samples.push_back(s);
	}
	input.samples = std::move(samples);

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	EXPECT_EQ(ApplyPlanStatus::NeedsConfirmation, plan.status);
	EXPECT_TRUE(plan.needs_confirmation());
	EXPECT_GT(plan.event_count, 100u);
	EXPECT_FALSE(plan.lines.empty()); // plan still produced for confirmation
}

TEST(motion_track_apply_plan, homography_model_is_hard_error) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, "x");
	auto input = BaseInput();
	input.model = TrackModel::Homography;
	input.samples = LinearSamples(0, 29, 0, 0, 1.0, 0.0);

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	EXPECT_EQ(ApplyPlanStatus::UnsupportedModel, plan.status);
	EXPECT_FALSE(plan.has_mutations());
}

namespace {
constexpr double kPi = 3.14159265358979323846;

void FillPose(TrackSample& s, double cx, double cy, double theta_cw,
              double scale) {
	s.status = TrackStatus::Ok;
	s.center_x = cx;
	s.center_y = cy;
	s.transform.matrix[0] = scale * std::cos(theta_cw);
	s.transform.matrix[1] = -scale * std::sin(theta_cw);
	s.transform.matrix[3] = scale * std::sin(theta_cw);
	s.transform.matrix[4] = scale * std::cos(theta_cw);
}
}

TEST(motion_track_apply_plan, similarity_compact_mode_is_rejected) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, "x");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Compact;
	input.samples = LinearSamples(0, 29, 0, 0, 1.0, 0.0);

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	EXPECT_EQ(ApplyPlanStatus::UnsupportedMode, plan.status);
	EXPECT_FALSE(plan.has_mutations());
	EXPECT_FALSE(plan.message.empty());
}

TEST(motion_track_apply_plan, similarity_exact_emits_full_transform_tags) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(120,80)}logo)");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 120.0, 100.0, kPi / 2, 1.05);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	// The line origin (120,80) sits at offset (20,-20) from the ROI seed
	// center (100,100); rotating that offset 90 deg clockwise at scale 1.05
	// gives (21,21), so the anchor lands at center+(21,21) = (141,121) --
	// NOT the translation-only (140,80). \frz is counterclockwise-positive,
	// so +90 deg clockwise emits -90. Style base scale is 100/100.
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts.front().text.find(
	              R"(\pos(141.00,121.00)\frz(-90.00)\fscx(105.00)\fscy(105.00))"));
}

TEST(motion_track_apply_plan, similarity_exact_composes_style_scale_and_angle) {
	Fixture fx;
	auto* style = new AssStyle;
	style->name = "Rot";
	style->scalex = 110;
	style->scaley = 90;
	style->angle = 10;
	fx.file.Styles.push_back(*style);
	auto* line = fx.AddLine(0, 2000, R"({\pos(100,100)}q)");
	line->Style = "Rot";

	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, kPi / 6.0, 0.5); // 30 deg cw, 50%
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	// \frz replaces the style Angle: 10 deg CCW base composed with 30 deg
	// CW tracked = -20. \fscx/\fscy replace the style percentages, so the
	// base multiplies in: 110*0.5, 90*0.5. Anchor offset is zero here.
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts.front().text.find(
	              R"(\pos(100.00,100.00)\frz(-20.00)\fscx(55.00)\fscy(45.00))"));
}

TEST(motion_track_apply_plan, similarity_exact_splits_parts_on_pose_change) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(0,0)}x)");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		if (f < 10)
			FillPose(s, 10.0, 0.0, 0.0, 1.0);
		else
			FillPose(s, 20.0, 0.0, kPi / 12.0, 1.1);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	size_t covered = 0;
	bool found_identity = false;
	bool found_turned = false;
	for (auto const& part : plan.lines[0].parts) {
		if (!part.covered) continue;
		++covered;
		// Identity pose emits no transform tags; the turned pose does.
		found_identity = found_identity
		    || (part.text.find("\\fscx") == std::string::npos
		        && part.text.find(R"(\pos(10.00,0.00))") != std::string::npos);
		found_turned = found_turned
		    || part.text.find(R"(\fscx(110.00))") != std::string::npos;
	}
	EXPECT_EQ(2u, covered);
	EXPECT_TRUE(found_identity);
	EXPECT_TRUE(found_turned);
}

TEST(motion_track_apply_plan, similarity_interior_gap_holds_full_pose) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(0,0)}x)");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 10.0, 0.0, 0.0, 1.0);
		samples.push_back(s);
	}
	MarkFailed(samples, 8, 10);
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	// Frames 8..10 hold frame 7's pose; the held tuple equals the tracked
	// one, and the anchor sits on the ROI center (\pos(0,0) == origin).
	// Identity pose emits \pos only — no transform tags anywhere.
	bool found_held_pose = false;
	for (auto const& part : plan.lines[0].parts) {
		found_held_pose = found_held_pose
		    || part.text.find(R"(\pos(10.00,0.00))") != std::string::npos;
		EXPECT_EQ(std::string::npos, part.text.find("\\frz"));
		EXPECT_EQ(std::string::npos, part.text.find("\\fscx"));
	}
	EXPECT_TRUE(found_held_pose);
}

TEST(motion_track_apply_plan, similarity_pure_translation_omits_transform_tags) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(0,0)}plain)");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 10.0 + f, 0.0, 0.0, 1.0); // translation only
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_FALSE(plan.lines[0].parts.empty());
	for (auto const& part : plan.lines[0].parts) {
		if (!part.covered) continue;
		EXPECT_EQ(std::string::npos, part.text.find("\\frz"));
		EXPECT_EQ(std::string::npos, part.text.find("\\fscx"));
		EXPECT_EQ(std::string::npos, part.text.find("\\fscy"));
		EXPECT_NE(std::string::npos, part.text.find("\\pos("));
	}
}

TEST(motion_track_apply_plan, similarity_preserves_inline_transform_base) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\frz45\fscx150\fscy80}styled)");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 10.0, 0.0, 0.0, 1.0); // no tracked rotation/scale
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	// The inline tags were stripped, so the base must be re-emitted or the
	// user's styling would be silently reset to the style values.
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts.front().text.find(R"(\frz(45.00))"));
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts.front().text.find(R"(\fscx(150.00))"));
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts.front().text.find(R"(\fscy(80.00))"));
}

TEST(motion_track_apply_plan, move_origin_interpolates_at_seed_time) {
	Fixture fx;
	// Whole-event window (t1/t2 omitted): 0..2000 ms maps (0,0)->(100,0).
	auto* line = fx.AddLine(0, 2000, R"({\move(0,0,100,0)}m)");
	auto input = BaseInput();
	input.seed_time_ms = 1000; // midpoint
	input.samples = LinearSamples(0, 29, 20, 0, 0.0, 0.0); // center=20, origin_center=10 -> delta +10
	input.origin_center_x = 10;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	// Origin at seed = (50,0); delta +10 -> (60,0) for every frame.
	EXPECT_NE(std::string::npos,
	          plan.lines[0].parts[0].text.find(R"(\pos(60.00,0.00))"))
	    << plan.lines[0].parts[0].text;
}

TEST(motion_track_apply_plan, reversed_explicit_move_window_is_invalid) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\move(0,0,100,0,50,10)}m)");
	auto input = BaseInput();
	input.seed_time_ms = 20;
	input.samples = LinearSamples(0, 29, 0, 0, 0.0, 0.0);

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, plan.status);
	EXPECT_FALSE(plan.has_mutations());
}

TEST(motion_track_apply_plan, resolve_dialogue_origin_variants) {
	Fixture fx;
	fx.file.GetStyle("Default")->alignment = 5;
	fx.file.GetStyle("Default")->Margin[0] = 100;
	fx.file.GetStyle("Default")->Margin[1] = 0;
	fx.file.GetStyle("Default")->Margin[2] = 0;

	double x = 0, y = 0;

	AssDialogue pos_line;
	pos_line.Text = R"({\pos(12,34)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, pos_line, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(12.0, x);
	EXPECT_DOUBLE_EQ(34.0, y);

	// Style/margin fallback with \an override: alignment 6 -> right-center?
	// an6 = middle-right: x = W - MarginR (=0), y = H/2.
	AssDialogue an_line;
	an_line.Text = R"({\an6}t)";
	an_line.Margin = {{100, 0, 0}};
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, an_line, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(1920.0, x);
	EXPECT_DOUBLE_EQ(540.0, y);

	// No tags: style alignment 5 (middle-center) with MarginL 100.
	AssDialogue plain;
	plain.Text = "t";
	plain.Margin = {{100, 0, 0}};
	ASSERT_TRUE(
	    ResolveDialogueOrigin(fx.file, plain, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ((1920 + 100 - 0) / 2.0, x);
	EXPECT_DOUBLE_EQ(1080 / 2.0, y);
}

TEST(motion_track_apply_plan, replace_position_tag_keeps_other_tags) {
	EXPECT_EQ(R"({\b1\pos(3.00,4.00)}go{\i1})",
	          ReplacePositionTag(R"({\b1\pos(1,2)}go{\i1})", R"(\pos(3.00,4.00))"));
	EXPECT_EQ(R"({\b1\move(0.00,0.00,9.00,9.00)}go)",
	          ReplacePositionTag(R"({\b1\move(1,1,2,2)}go)", R"(\move(0.00,0.00,9.00,9.00))"));
	EXPECT_EQ(R"({\pos(1.00,2.00)}plain)",
	          ReplacePositionTag("plain", R"(\pos(1.00,2.00))"));
	// Both tags present: both stripped, one replacement inserted.
	EXPECT_EQ(R"({\pos(5.00,5.00)}x)",
	          ReplacePositionTag(R"({\pos(1,1)\move(2,2,3,3)}x)", R"(\pos(5.00,5.00))"));
}

TEST(motion_track_apply_plan, compact_pieces_stay_within_epsilon_and_continuous) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(100,100)}curve)");
	auto input = BaseInput();
	// x accelerates at 0.25 px/frame^2: genuinely curved, so Compact must
	// add knots rather than collapse to a single segment.
	std::vector<TrackSample> samples;
	for (int f = 0; f <= 29; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		s.center_x = 0.25 * f * f; // storage px == script px here
		s.center_y = 0.0;
		samples.push_back(s);
	}
	input.samples = std::move(samples);
	input.origin_center_x = 0;
	input.origin_center_y = 0;
	input.options.compact_epsilon = 1.5;
	input.options.position_decimals = 4;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	auto const& parts = plan.lines[0].parts;
	ASSERT_GE(parts.size(), 2u) << "a curve must force multiple segments";
	// The backward-elimination pass must keep the fit minimal: a smooth
	// 0.25 px/frame^2 curve over 20 frames needs a handful of segments, not
	// one per frame.
	size_t covered_parts = 0;
	for (auto const& part : parts)
		if (part.covered) ++covered_parts;
	EXPECT_LE(covered_parts, 6u) << "over-segmentation regression";

	// Every domain frame renders within epsilon of its sample position.
	for (int f = 0; f <= 19; ++f) {
		double px = 0, py = 0;
		ASSERT_TRUE(PositionAtFrame(parts, input.timecodes, f, px, py))
		    << "frame " << f;
		EXPECT_NEAR(100.0 + 0.25 * f * f, px, 1.5 + 1e-6) << "frame " << f;
		EXPECT_NEAR(100.0, py, 1.5 + 1e-6) << "frame " << f;
	}

	// Covered parts tile the timeline without gaps or overlaps. (Adjacent
	// parts' TAG endpoints differ by one frame of motion by design: each part
	// starts at the spline value at its own start time, so every rendered
	// frame sits on the spline — the per-frame check above is the contract.)
	PlannedLinePart const* prev = nullptr;
	for (auto const& part : parts) {
		if (!part.covered) continue;
		if (prev) EXPECT_EQ(prev->end_ms, part.start_ms);
		prev = &part;
	}
}

TEST(motion_track_apply_plan, smooth_preserves_constant_velocity_exactly) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(50,60)}slide)");
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 0, 0, 2.0, 0.0);
	input.origin_center_x = 0;
	input.origin_center_y = 0;
	input.options.mode = ApplyMode::Exact;
	input.options.position_decimals = 4;
	input.options.smooth_frames = 5;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	for (int f = 0; f <= 19; ++f) {
		double px = 0, py = 0;
		ASSERT_TRUE(PositionAtFrame(plan.lines[0].parts, input.timecodes, f,
		                            px, py))
		    << "frame " << f;
		EXPECT_NEAR(50.0 + 2.0 * f, px, 1e-6) << "frame " << f;
		EXPECT_NEAR(60.0, py, 1e-6) << "frame " << f;
	}
}

TEST(motion_track_apply_plan, smooth_attenuates_noise_in_exact_mode) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(50,60)}jitter)");
	auto input = BaseInput();
	std::vector<TrackSample> samples;
	for (int f = 0; f <= 29; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		s.center_x = 2.0 * f + (f % 2 ? 1.0 : 0.0); // +-1 px jitter
		s.center_y = 0;
		samples.push_back(s);
	}
	input.samples = std::move(samples);
	input.origin_center_x = 0;
	input.origin_center_y = 0;
	input.options.mode = ApplyMode::Exact;
	input.options.position_decimals = 4;

	double max_dev[2] = {0.0, 0.0};
	for (int pass = 0; pass < 2; ++pass) {
		input.options.smooth_frames = pass == 0 ? 0 : 4;
		auto plan = BuildApplyPlan(fx.file, {line}, input);
		ASSERT_TRUE(plan.has_mutations());
		for (int f = 0; f <= 19; ++f) {
			double px = 0, py = 0;
			ASSERT_TRUE(PositionAtFrame(plan.lines[0].parts, input.timecodes,
			                            f, px, py))
			    << "frame " << f;
			// Deviation from the CLEAN line: smoothing must pull positions
			// toward the underlying motion, not reproduce the noise.
			max_dev[pass] = std::max(max_dev[pass],
			                         std::abs(px - (50.0 + 2.0 * f)));
		}
	}
	EXPECT_NEAR(1.0, max_dev[0], 1e-9); // smoothing off: noise passes through
	EXPECT_LT(max_dev[1], 0.75);        // smoothing on: clearly attenuated
	                                      // (edge frames see a truncated
	                                      // window, so not 0)
}

TEST(motion_track_apply_plan, smoothing_never_crosses_hold_boundaries) {
	Fixture fx;
	auto* line = fx.AddLine(0, 2000, R"({\pos(0,0)}track)");
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 10, 20, 3.0, 0.0);
	MarkFailed(input.samples, 5, 8);
	input.options.compact_epsilon = 0.01;
	input.options.smooth_frames = 10; // aggressive: must not touch the hold

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	int const gap_start = input.timecodes.TimeAtFrame(5, agi::vfr::Time::START);
	int const gap_end = input.timecodes.TimeAtFrame(9, agi::vfr::Time::START);
	bool found_hold = false;
	for (auto const& part : plan.lines[0].parts) {
		if (part.start_ms == gap_start && part.end_ms == gap_end) {
			found_hold = true;
			EXPECT_NE(std::string::npos,
			          part.text.find(R"(\pos(22.00,20.00))"))
			    << PartSummary(part);
		}
	}
	EXPECT_TRUE(found_hold);
}

TEST(motion_track_apply_plan, compact_epsilon_measured_in_storage_pixels) {
	// Identical script-space geometry (one-frame 1 script px bump on a linear
	// slide); only the script/storage ratio changes. A storage-space threshold
	// must keep the bump when the video is bigger than the script grid and
	// drop it when the video is smaller.
	auto run_case = [&](int script_w, int script_h, double bump_storage) {
		Fixture fx;
		auto* line = fx.AddLine(0, 2000, R"({\pos(100,100)}bump)");
		auto input = BaseInput();
		input.script_width = script_w;
		input.script_height = script_h;
		std::vector<TrackSample> samples;
		for (int f = 0; f <= 29; ++f) {
			TrackSample s;
			s.frame = f;
			s.status = TrackStatus::Ok;
			s.center_x = 2.0 * f + (f == 15 ? bump_storage : 0.0);
			s.center_y = 0;
			samples.push_back(s);
		}
		input.samples = std::move(samples);
		input.origin_center_x = 0;
		input.origin_center_y = 0;
		input.options.compact_epsilon = 0.75;
		auto plan = BuildApplyPlan(fx.file, {line}, input);
		return plan.has_mutations() ? plan.event_count : 0u;
	};

	EXPECT_GE(run_case(1920, 1080, 1.0), 2u); // 1 script px = 1 storage px
	EXPECT_GE(run_case(960, 540, 2.0), 2u);   // 1 script px = 2 storage px
	EXPECT_EQ(1u, run_case(3840, 2160, 0.5)); // 1 script px = 0.5 storage px
}
