#include <main.h>

#include "../../src/motion_track/apply_plan.h"
#include "../../src/motion_track/types.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>

#include <libaegisub/ass/time.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace aegisub::motion_track;

struct Fixture {
	AssFile file;
	agi::vfr::Framerate fps = agi::vfr::Framerate(10.0); // 100 ms frames
	std::vector<AssDialogue *> lines;

	Fixture() {
		auto *style = new AssStyle;
		style->name = "Default";
		file.Styles.push_back(*style);
	}

	AssDialogue *AddLine(int start_ms, int end_ms, std::string text,
						 bool comment = false) {
		auto *line = new AssDialogue;
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
		if (s.frame >= first && s.frame <= last)
			s.status = TrackStatus::Failed;
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
		if (i >= s.size() || s[i] != ',')
			return false;
		++i;
		return true;
	};
	auto parse_double = [&](std::string const& s, size_t& i, double& v)
		-> bool {
		char *end = nullptr;
		v = std::strtod(s.c_str() + i, &end);
		if (end == s.c_str() + i)
			return false;
		i = size_t(end - s.c_str());
		return true;
	};
	auto parse_int = [&](std::string const& s, size_t& i, int& v) -> bool {
		char *end = nullptr;
		v = int(std::strtol(s.c_str() + i, &end, 10));
		if (end == s.c_str() + i)
			return false;
		i = size_t(end - s.c_str());
		return true;
	};

	size_t at = text.find("\\move(");
	if (at != std::string::npos) {
		size_t i = at + 6;
		out.is_move = true;
		return parse_double(text, i, out.x1) && next_comma(text, i) && parse_double(text, i, out.y1) && next_comma(text, i) && parse_double(text, i, out.x2) && next_comma(text, i) && parse_double(text, i, out.y2) && next_comma(text, i) && parse_int(text, i, out.t1) && next_comma(text, i) && parse_int(text, i, out.t2);
	}
	at = text.find("\\pos(");
	if (at != std::string::npos) {
		size_t i = at + 5;
		out.is_move = false;
		out.t1 = out.t2 = 0;
		return parse_double(text, i, out.x1) && next_comma(text, i) && parse_double(text, i, out.y1) && (out.x2 = out.x1, out.y2 = out.y1, true);
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
		if (!part.covered)
			continue;
		if (tc < part.start_ms || tc >= part.end_ms)
			continue;
		TagPos tag;
		if (!ParseTag(part.text, tag))
			return false;
		if (!tag.is_move || tc - part.start_ms <= tag.t1) {
			x = tag.x1;
			y = tag.y1;
		}
		else if (tc - part.start_ms >= tag.t2) {
			x = tag.x2;
			y = tag.y2;
		}
		else {
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,200)}hello)");
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(50,60)}slide)");
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
	// at 0ms -- emitted as t1 = 0, which libass honors as an explicit window
	// (only both bounds non-positive degrade) -- and frame 19 renders at
	// 1900ms.
	bool found_move = false;
	for (auto const& part : plan.lines[0].parts)
		found_move = found_move || part.text.find(R"(\move(50.00,60.00,88.00,60.00,0,1900))") != std::string::npos;
	EXPECT_TRUE(found_move) << PartSummary(plan.lines[0].parts.front());
}

TEST(motion_track_apply_plan, covered_part_geometry_matches_emitted_tags) {
	Fixture fx;
	auto *line = fx.AddLine(0, 3000, "hello");
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
		if (!part.covered)
			continue; // vfr END rounding leaves a tail
		++covered_count;
		TagPos tag;
		ASSERT_TRUE(ParseTag(part.text, tag));
		// The geometry fields carry the exact fitted positions; the tag text
		// rounds them to position_decimals, so compare within that rounding.
		double const tol = 0.5 * std::pow(10.0, -input.options.position_decimals) + 1e-9;
		EXPECT_NEAR(tag.x1, part.x0, tol);
		EXPECT_NEAR(tag.y1, part.y0, tol);
		if (tag.is_move) {
			EXPECT_NEAR(tag.x2, part.x1, tol);
			EXPECT_NEAR(tag.y2, part.y1, tol);
		}
		else {
			EXPECT_DOUBLE_EQ(part.x0, part.x1);
			EXPECT_DOUBLE_EQ(part.y0, part.y1);
		}
	}
	// The curvature must have forced more than one move segment.
	EXPECT_GE(covered_count, 2u);
}

TEST(motion_track_apply_plan, interior_failed_gap_holds_previous_ok_position) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(0,0)}track)");
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
		auto *line = fx.AddLine(0, 2000, "hi");
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
		auto *line = fx.AddLine(0, 3500, "hi"); // domain reaches frame 29
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(0,0)}x)");
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
	auto *comment = fx.AddLine(0, 2000, "commentary", /*comment=*/true);
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
	auto *full = fx.AddLine(0, 3000, R"({\pos(0,0)}full)");
	auto *part = fx.AddLine(1000, 3000, R"({\pos(0,0)}part)");
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
		full_ok = full_ok || p.text.find(R"(\pos(0.00,0.00))") != std::string::npos;
	for (auto const& p : plan.lines[1].parts)
		part_ok = part_ok || p.text.find(R"(\pos(30.00,0.00))") != std::string::npos;
	EXPECT_TRUE(full_ok);
	EXPECT_TRUE(part_ok);
	// Nothing from before frame 10 leaks into the short line.
	for (auto const& p : plan.lines[1].parts) {
		if (!p.covered)
			continue;
		TagPos tag;
		ASSERT_TRUE(ParseTag(p.text, tag));
		EXPECT_GE(tag.x1, 30.0 - 0.001);
	}
}

TEST(motion_track_apply_plan, out_of_domain_prefix_and_suffix_preserved) {
	Fixture fx;
	auto *line = fx.AddLine(500, 2500, R"({\pos(0,0)}keep me)");
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

TEST(motion_track_apply_plan, sub_centisecond_suffix_is_not_emitted) {
	// 23.976 fps: frame 98's END boundary is 4108, off the centisecond grid,
	// so a line ending at 4110 leaves a 2ms uncovered tail. Both boundaries
	// round onto centisecond 4110, so the sliver would serialize as a
	// zero-duration duplicate event carrying the original text; it must be
	// skipped instead.
	Fixture fx;
	auto *line = fx.AddLine(1000, 4110, R"({\pos(0,0)}tail sliver)");
	auto input = BaseInput();
	input.timecodes = agi::vfr::Framerate(23.976);
	input.decode_interval = FrameInterval{0, 149};
	input.direction_domain = FrameInterval{0, 149};
	input.video_frame_count = 150;
	input.samples = LinearSamples(0, 149, 0, 0, 0.0, 0.0);
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(1u, parts.size());
	EXPECT_TRUE(parts[0].covered);
	EXPECT_EQ(1000, parts[0].start_ms);
	EXPECT_EQ(4108, parts[0].end_ms); // frame 98's END boundary
	EXPECT_EQ(1u, plan.event_count);
	// The dropped sliver is below serialization granularity: the covered
	// part's rounded end already reaches the line's rounded end.
	EXPECT_EQ(int(agi::Time(line->End)), int(agi::Time(parts[0].end_ms)));
}

TEST(motion_track_apply_plan, sub_centisecond_prefix_is_not_emitted) {
	// START(7) is 271 at 23.976 fps; a line starting at 270 leaves a 1ms
	// uncovered head sliver that rounds onto centisecond 270 and must not be
	// emitted. The 19ms tail (frame 47's END = 1981) is a real uncovered
	// span and stays.
	Fixture fx;
	auto *line = fx.AddLine(270, 2000, R"({\pos(0,0)}head sliver)");
	auto input = BaseInput();
	input.timecodes = agi::vfr::Framerate(23.976);
	input.decode_interval = FrameInterval{0, 149};
	input.direction_domain = FrameInterval{0, 149};
	input.video_frame_count = 150;
	input.samples = LinearSamples(0, 149, 0, 0, 0.0, 0.0);
	input.origin_center_x = 0;
	input.origin_center_y = 0;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	auto const& parts = plan.lines[0].parts;
	ASSERT_EQ(2u, parts.size());
	EXPECT_TRUE(parts[0].covered);
	EXPECT_EQ(271, parts[0].start_ms); // frame 7's START boundary
	// No serialized gap: the covered part's rounded start reaches the line's.
	EXPECT_EQ(int(agi::Time(line->Start)), int(agi::Time(parts[0].start_ms)));
	EXPECT_FALSE(parts[1].covered);
	EXPECT_EQ(1981, parts[1].start_ms);
	EXPECT_EQ(2000, parts[1].end_ms);
	EXPECT_EQ(line->Text, parts[1].text);
}

TEST(motion_track_apply_plan, exact_over_hundred_events_needs_confirmation) {
	Fixture fx;
	auto *line = fx.AddLine(0, 12500, "x");
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
	auto *line = fx.AddLine(0, 2000, "x");
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
	auto *line = fx.AddLine(0, 2000, "x");
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(120,80)}logo)");
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
	auto *style = new AssStyle;
	style->name = "Rot";
	style->scalex = 110;
	style->scaley = 90;
	style->angle = 10;
	fx.file.Styles.push_back(*style);
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}q)");
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(0,0)}x)");
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
		if (!part.covered)
			continue;
		++covered;
		// Identity pose emits no transform tags; the turned pose does.
		found_identity = found_identity || (part.text.find("\\fscx") == std::string::npos && part.text.find(R"(\pos(10.00,0.00))") != std::string::npos);
		found_turned = found_turned || part.text.find(R"(\fscx(110.00))") != std::string::npos;
	}
	EXPECT_EQ(2u, covered);
	EXPECT_TRUE(found_identity);
	EXPECT_TRUE(found_turned);
}

TEST(motion_track_apply_plan, similarity_interior_gap_holds_full_pose) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(0,0)}x)");
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
		found_held_pose = found_held_pose || part.text.find(R"(\pos(10.00,0.00))") != std::string::npos;
		EXPECT_EQ(std::string::npos, part.text.find("\\frz"));
		EXPECT_EQ(std::string::npos, part.text.find("\\fscx"));
	}
	EXPECT_TRUE(found_held_pose);
}

TEST(motion_track_apply_plan, similarity_pure_translation_omits_transform_tags) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(0,0)}plain)");
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
		if (!part.covered)
			continue;
		EXPECT_EQ(std::string::npos, part.text.find("\\frz"));
		EXPECT_EQ(std::string::npos, part.text.find("\\fscx"));
		EXPECT_EQ(std::string::npos, part.text.find("\\fscy"));
		EXPECT_NE(std::string::npos, part.text.find("\\pos("));
	}
}

TEST(motion_track_apply_plan, similarity_preserves_inline_transform_base) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\frz45\fscx150\fscy80}styled)");
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
	EXPECT_EQ(std::string::npos,
			  plan.lines[0].parts.front().text.find("\\frz45"));
	EXPECT_NE(std::string::npos,
			  plan.lines[0].parts.front().text.find(R"(\fscx(150.00))"));
	EXPECT_NE(std::string::npos,
			  plan.lines[0].parts.front().text.find(R"(\fscy(80.00))"));
}

TEST(motion_track_apply_plan, move_origin_interpolates_at_seed_time) {
	Fixture fx;
	// Whole-event window (t1/t2 omitted): 0..2000 ms maps (0,0)->(100,0).
	auto *line = fx.AddLine(0, 2000, R"({\move(0,0,100,0)}m)");
	auto input = BaseInput();
	input.seed_time_ms = 1000;                             // midpoint
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

TEST(motion_track_apply_plan, reversed_explicit_move_window_swaps_like_libass) {
	// libass swaps t1 > t2 unconditionally before the both-non-positive
	// fallback, so this window renders as [10, 50]: rel = 20 -> t = 0.25 ->
	// origin (25, 0) -- not the whole-event window (which would give
	// (10, 0)) and not a rejection.
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\move(0,0,100,0,50,10)}m)");
	auto input = BaseInput();
	input.seed_time_ms = 20;
	input.samples = LinearSamples(0, 29, 0, 0, 0.0, 0.0);

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(1u, plan.lines.size());
	EXPECT_NE(std::string::npos,
			  plan.lines[0].parts[0].text.find(R"(\pos(25.00,0.00))"))
		<< plan.lines[0].parts[0].text;
}

TEST(motion_track_apply_plan, explicit_move_window_with_zero_t1_is_honored) {
	// A window with any positive bound is explicit: only BOTH bounds
	// non-positive (or a 4-arg move) spans the whole event. The old
	// "w1 <= 0 || w2 <= 0" test degraded \move(...,0,500) to the whole line
	// and dragged every part's origin off by the same constant offset.
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\move(0,0,100,0,0,500)}m)");

	double x = 0.0, y = 0.0;
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, *line, 250, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(50.0, x); // window [0, 500]: t = 0.5 (was 12.5)
	EXPECT_DOUBLE_EQ(0.0, y);
}

TEST(motion_track_apply_plan, deeply_nested_transform_does_not_overflow_stack) {
	// ScanRawPositions recurses into \t regions; 5000 nested \t( must hit
	// the depth cap instead of exhausting the stack. Levels beyond the cap
	// are invisible, so the \pos falls through to the style fallback and
	// the resolver still succeeds.
	std::string text = "{";
	for (int i = 0; i < 5000; ++i)
		text += R"(\t(0,100,)";
	text += R"(\pos(1,2))";
	for (int i = 0; i < 5000; ++i)
		text += ")";
	text += "}x";

	Fixture fx;
	auto *line = fx.AddLine(0, 2000, text);
	double x = 0.0, y = 0.0;
	EXPECT_TRUE(ResolveDialogueOrigin(fx.file, *line, 0, 1920, 1080, x, y));
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

TEST(motion_track_apply_plan, legacy_a4_a8_align_top_left) {
	Fixture fx;
	fx.file.GetStyle("Default")->Margin = {{0, 0, 0}};
	double x = 0, y = 0;
	// Tag-level \a4/\a8 are VSFilter-illegal and libass maps them like \a5
	// (\an7, top-left). SsaToAss's default of 2 would put this at the bottom.
	AssDialogue a4;
	a4.Text = R"({\a4}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, a4, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);
	AssDialogue a8;
	a8.Text = R"({\a8}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, a8, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);
}

TEST(motion_track_apply_plan, script_resolution_uses_playres_not_layoutres) {
	Fixture fx;
	fx.file.SetScriptInfo("PlayResX", "1920");
	fx.file.SetScriptInfo("PlayResY", "1080");
	fx.file.SetScriptInfo("LayoutResX", "1280");
	fx.file.SetScriptInfo("LayoutResY", "720");

	ApplyPlanInput input;
	FillApplyInputScriptResolution(input, fx.file);
	EXPECT_EQ(1920, input.script_width);
	EXPECT_EQ(1080, input.script_height);

	auto *line = fx.AddLine(0, 2000, R"({\an5}center)");
	input = BaseInput();
	FillApplyInputScriptResolution(input, fx.file);
	input.storage_width = 1280;
	input.storage_height = 720;
	input.origin_center_x = 640;
	input.origin_center_y = 360;
	input.samples = LinearSamples(0, 19, 640, 360, 0.0, 0.0);
	input.options.mode = ApplyMode::Exact;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_FALSE(plan.lines.empty());
	ASSERT_FALSE(plan.lines[0].parts.empty());
	// Storage center (640,360) at PlayRes 1920x1080 with \an5 is the script
	// center (960,540). Using LayoutRes would have written (640,360).
	EXPECT_NE(std::string::npos,
			  plan.lines[0].parts.front().text.find(R"(\pos(960.00,540.00))"));
	EXPECT_EQ(std::string::npos,
			  plan.lines[0].parts.front().text.find(R"(\pos(640.00,360.00))"));
}

TEST(motion_track_apply_plan, dialogue_copy_keeps_extradata_ids) {
	AssDialogue source;
	source.Layer = 3;
	source.Actor = "n";
	source.Effect = "karaoke";
	source.ExtradataIds = std::vector<uint32_t>{7, 11};
	source.Text = "keep me";
	AssDialogue copy(source);
	EXPECT_EQ(source.Layer, copy.Layer);
	EXPECT_EQ(source.Actor.get(), copy.Actor.get());
	EXPECT_EQ(source.Effect.get(), copy.Effect.get());
	EXPECT_EQ(source.ExtradataIds.get(), copy.ExtradataIds.get());
	EXPECT_EQ(source.Text.get(), copy.Text.get());
	EXPECT_NE(source.Id, copy.Id);
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
	// Nested \t must keep its closing paren; a scanner that eats the first
	// ')' would emit unbalanced ASS.
	std::string const nested =
		ReplacePositionTag(R"({\t(0,100,\frz)}x)", R"(\pos(1.00,2.00))");
	EXPECT_NE(std::string::npos, nested.find(R"(\t(0,100,)"));
	EXPECT_EQ(std::count(nested.begin(), nested.end(), '('),
			  std::count(nested.begin(), nested.end(), ')'));
	EXPECT_NE(std::string::npos, nested.find(R"(\pos(1.00,2.00))"));
}

TEST(motion_track_apply_plan, replace_position_tag_preserves_untouched_tag_bytes) {
	// libass accepts a seven-parameter \fad while the proto table declares
	// two: re-serializing the block through the parser truncated the extra
	// parameters even though motion track never touches the fade.
	EXPECT_EQ(R"({\fad(255,0,255,0,100,900,1000)\pos(3.00,4.00)}x)",
			  ReplacePositionTag(
				  R"({\fad(255,0,255,0,100,900,1000)\pos(1,2)}x)",
				  R"(\pos(3.00,4.00))"));
	// The style name after \r keeps its leading space byte-for-byte.
	EXPECT_EQ(R"({\r Alt\pos(3.00,4.00)}x)",
			  ReplacePositionTag(R"({\r Alt\pos(1,2)}x)",
								 R"(\pos(3.00,4.00))"));
	// Untouched later blocks also keep their original spelling.
	EXPECT_EQ(R"({\pos(3.00,4.00)}x{\fn Some Font\b0})",
			  ReplacePositionTag(R"({\pos(1,2)}x{\fn Some Font\b0})",
								 R"(\pos(3.00,4.00))"));
}

TEST(motion_track_apply_plan, replace_position_tag_leads_surviving_hidden_position) {
	// libass skips spaces after '\', so "\ pos(10,20)" is a real position tag
	// even though the proto-table drop pass classifies it as junk. pos/move
	// resolve first-wins per event, so the replacement must precede the kept
	// bytes whenever such a spelling survives the drop pass.
	EXPECT_EQ(R"({\pos(30.00,40.00)\ pos(10,20)}x)", ReplacePositionTag(R"({\ pos(10,20)}x)", R"(\pos(30.00,40.00))"));
	EXPECT_EQ(R"({\move(9.00,8.00,7.00,6.00)\ move(1,1,2,2)}x)", ReplacePositionTag(R"({\ move(1,1,2,2)}x)", R"(\move(9.00,8.00,7.00,6.00))"));
}

TEST(motion_track_apply_plan, replace_position_tag_leads_t_nested_position) {
	// A \t argument is parsed as tags again and its nested \pos takes effect
	// immediately (event-wide first-wins), so the replacement must land
	// before the preserved \t block while the \t itself stays intact.
	EXPECT_EQ(R"({\pos(30.00,40.00)\t(0,100,\pos(10,20))}x)", ReplacePositionTag(R"({\t(0,100,\pos(10,20))}x)", R"(\pos(30.00,40.00))"));
}

TEST(motion_track_apply_plan, replace_position_tag_drops_pos_after_leading_paren) {
	// Regression: the span split must start at index 1 like
	// AssDialogueBlockOverride::ParseTags, so the leading "(" cannot open a
	// paren region that shields the old \pos from the drop pass. With the
	// bug the stale pos survived the rewrite and libass's first-wins
	// resolution made the applied position a silent no-op.
	EXPECT_EQ(R"({(\p1\pos(30.00,40.00)}x)", ReplacePositionTag(R"({(\pos(10,20))\p1}x)", R"(\pos(30.00,40.00))"));
}

TEST(motion_track_apply_plan, resolve_dialogue_origin_sees_hidden_pos_spellings) {
	Fixture fx;
	double x = 0, y = 0;

	// The space-prefixed spelling is a real \pos for libass.
	AssDialogue spaced;
	spaced.Text = R"({\ pos(12,34)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, spaced, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(12.0, x);
	EXPECT_DOUBLE_EQ(34.0, y);

	// A \t-nested \pos takes effect immediately and must be extracted too.
	AssDialogue nested;
	nested.Text = R"({\t(0,100,\pos(56,78))}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, nested, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(56.0, x);
	EXPECT_DOUBLE_EQ(78.0, y);

	// Textual first-wins across spellings: the hidden first pos shadows the
	// later canonical one (the parsed-tag source used to return (1,2)).
	AssDialogue ordered;
	ordered.Text = R"({\ pos(3,4)\pos(1,2)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, ordered, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(3.0, x);
	EXPECT_DOUBLE_EQ(4.0, y);

	// libass argtod turns unconvertible tokens into 0 and still occupies
	// EVENT_POSITIONED, so the later well-formed \pos must not win.
	AssDialogue broken;
	broken.Text = R"({\pos(a,b)\pos(5,6)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, broken, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);

	// \move nested in \t, with the explicit integer [t1,t2] window.
	AssDialogue nested_move;
	nested_move.Text = R"({\t(0,100,\move(10,20,30,40,50,60))}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, nested_move, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(10.0, x); // rel=0 <= t1=50: start point
	EXPECT_DOUBLE_EQ(20.0, y);
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, nested_move, 55, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(20.0, x); // midway through the [50,60] window
	EXPECT_DOUBLE_EQ(30.0, y);

	// Prefix name: \position is \pos for libass (mystrcmp).
	AssDialogue position;
	position.Text = R"({\position(12,34)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, position, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(12.0, x);
	EXPECT_DOUBLE_EQ(34.0, y);

	// Space between the tag name and '('.
	AssDialogue spaced_paren;
	spaced_paren.Text = R"({\pos (12,34)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, spaced_paren, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(12.0, x);
	EXPECT_DOUBLE_EQ(34.0, y);

	// Wrong arity does not occupy the slot (libass requires nargs == 2 / 4 or 6).
	AssDialogue three_pos;
	three_pos.Text = R"({\pos(1,2,3)\pos(5,6)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, three_pos, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(5.0, x);
	EXPECT_DOUBLE_EQ(6.0, y);

	AssDialogue seven_move;
	seven_move.Text = R"({\move(1,2,3,4,5,6,7)\pos(5,6)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, seven_move, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(5.0, x);
	EXPECT_DOUBLE_EQ(6.0, y);

	// Parentheses do not nest: an unknown tag's first ')' leaves a trailing
	// \pos that libass then parses.
	AssDialogue leftover;
	leftover.Text = R"({\foo(\bar(1)\pos(12,34))}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, leftover, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(12.0, x);
	EXPECT_DOUBLE_EQ(34.0, y);
}

// pos and move share libass's EVENT_POSITIONED slot: the first valid
// instance of either kind blocks later instances of both, including the
// spellings only the raw scan can see.
TEST(motion_track_apply_plan, resolve_dialogue_origin_position_slot_is_shared_between_pos_and_move) {
	Fixture fx;
	double x = 0, y = 0;

	// The earlier \move wins over a later hidden \pos spelling; libass
	// ignores the \pos entirely.
	AssDialogue move_first;
	move_first.Text = R"({\move(10,20,30,40,0,100)\ pos(99,99)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, move_first, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(10.0, x);
	EXPECT_DOUBLE_EQ(20.0, y);

	// Same for a \t-nested \pos after a top-level \move.
	AssDialogue move_first_nested;
	move_first_nested.Text = R"({\move(10,20,30,40,0,100)\t(0,100,\pos(9,9))}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, move_first_nested, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(10.0, x);
	EXPECT_DOUBLE_EQ(20.0, y);

	// And the other direction: a first \pos blocks a later \move.
	AssDialogue pos_first;
	pos_first.Text = R"({\pos(7,8)\move(10,20,30,40,0,100)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, pos_first, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(7.0, x);
	EXPECT_DOUBLE_EQ(8.0, y);
}

// libass converts \pos arguments with ass_strtod, a digit/exponent parser
// with no inf/nan path, so these subjects fail to convert and argtod leaves
// the zero -- the tag still occupies EVENT_POSITIONED. A bare
// std::from_chars would instead hand back nan/inf, and nothing downstream
// of the resolved origin tests for finiteness: FormatCoord is snprintf
// "%.*f", so a NaN would be written back into the file as "\pos(nan,...)".
TEST(motion_track_apply_plan, non_finite_position_arguments_convert_to_zero) {
	Fixture fx;
	double x = -1, y = -1;

	AssDialogue nan_pos;
	nan_pos.Text = R"({\pos(nan,5)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, nan_pos, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(5.0, y);

	// "infinity" is the longer spelling from_chars also accepts.
	AssDialogue inf_pos;
	inf_pos.Text = R"({\pos(6,infinity)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, inf_pos, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(6.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);

	// An overflowing exponent is a conversion failure too: from_chars
	// reports result_out_of_range and MSVC still writes inf through the
	// out-param, so ignoring the error code would leak that inf.
	AssDialogue overflow_pos;
	overflow_pos.Text = R"({\pos(1e400,3)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, overflow_pos, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(3.0, y);

	// The slot is still taken: a later well-formed \pos does not win.
	AssDialogue nan_then_valid;
	nan_then_valid.Text = R"({\pos(nan,nan)\pos(11,22)}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, nan_then_valid, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);

	// Same conversion on the \move path, including its integer bounds --
	// ArgToInt keeps its own parse, so a non-numeric window bound is 0 and
	// both-non-positive falls back to the whole-event window.
	AssDialogue nan_move;
	nan_move.Text = R"({\move(nan,10,inf,20,nan,nan)}t)";
	nan_move.Start = 0;
	nan_move.End = 1000;
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, nan_move, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(10.0, y);
}

TEST(motion_track_apply_plan, alignment_seen_flag_blocks_later_an) {
	Fixture fx;
	fx.file.GetStyle("Default")->alignment = 5;
	fx.file.GetStyle("Default")->Margin = {{0, 0, 0}};
	double x = 0, y = 0;

	// \a12 is out of range: libass falls back to the style alignment but the
	// PARSED_A slot is still taken, so the later \an7 must be ignored.
	AssDialogue a12;
	a12.Text = R"({\a12\an7}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, a12, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(1920 / 2.0, x); // style alignment 5: middle-center
	EXPECT_DOUBLE_EQ(1080 / 2.0, y);

	// A valid \a6 converts to \an8 (top-center) and keeps first-wins over
	// the later \an7.
	AssDialogue a6;
	a6.Text = R"({\a6\an7}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, a6, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(1920 / 2.0, x); // an8: horizontally centered
	EXPECT_DOUBLE_EQ(0.0, y);        // an8: top margin

	// First seen wins in the other direction too: an7 stays despite the
	// later legal \a6.
	AssDialogue an7;
	an7.Text = R"({\an7\a6}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, an7, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x); // an7: left margin
	EXPECT_DOUBLE_EQ(0.0, y); // an7: top margin

	// Space after '\' is skipped by libass, so "\ an7" consumes PARSED_A.
	AssDialogue spaced_an;
	spaced_an.Text = R"({\ an7}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, spaced_an, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);

	// And it still first-wins over a later canonical \an.
	AssDialogue spaced_then_an;
	spaced_then_an.Text = R"({\ an7\an5}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, spaced_then_an, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);

	// Space between the name and the value is the other half: libass reaches
	// the conversion through strtoll, which skips it, so "\an 7" is alignment
	// 7 and not the out-of-range 0 that would fall back to the style.
	AssDialogue an_spaced_value;
	an_spaced_value.Text = R"({\an 7}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, an_spaced_value, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x); // an7: left margin, not style 5's center
	EXPECT_DOUBLE_EQ(0.0, y);

	// Legacy \a takes the same path (ass_strtod skips spaces as well): \a 6
	// is top-center, not the style fallback.
	AssDialogue a_spaced_value;
	a_spaced_value.Text = R"({\a 6}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, a_spaced_value, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(1920 / 2.0, x); // an8: horizontally centered
	EXPECT_DOUBLE_EQ(0.0, y);        // an8: top margin, not style 5's middle

	// Reading it as 0 would still consume the slot, so the later legal \an5
	// would be swallowed and the line would silently land style-aligned.
	AssDialogue spaced_value_then_an;
	spaced_value_then_an.Text = R"({\an 7\an5}t)";
	ASSERT_TRUE(ResolveDialogueOrigin(fx.file, spaced_value_then_an, 0, 1920, 1080, x, y));
	EXPECT_DOUBLE_EQ(0.0, x);
	EXPECT_DOUBLE_EQ(0.0, y);
}

TEST(motion_track_apply_plan, similarity_apply_leads_pos_over_surviving_stale_position) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\ pos(10,20)}logo)");
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
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_FALSE(plan.lines.empty());
	ASSERT_FALSE(plan.lines[0].parts.empty());
	std::string const& text = plan.lines[0].parts.front().text;
	// The raw scan feeds the origin (10,20), so the identity pose moves the
	// anchor to (20,20). The new \pos must precede the stale "\ pos" the
	// drop pass cannot see (libass pos first-wins), and the identity pose
	// emits no transform tags.
	size_t const fresh = text.find(R"(\pos(20.00,20.00))");
	size_t const stale = text.find(R"(\ pos(10,20))");
	ASSERT_NE(std::string::npos, fresh) << text;
	ASSERT_NE(std::string::npos, stale) << text;
	EXPECT_LT(fresh, stale);
	EXPECT_EQ(std::string::npos, text.find("\\frz")) << text;
	EXPECT_EQ(std::string::npos, text.find("\\fscx")) << text;
}

TEST(motion_track_apply_plan, similarity_inline_fr_absorbed_and_transforms_stay_last) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\b1\fr30\pos(0,0)}q)");
	auto input = BaseInput();
	input.model = TrackModel::Similarity;
	input.options.mode = ApplyMode::Exact;
	input.origin_center_x = 0;
	input.origin_center_y = 0;
	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 0.0, 0.0, kPi / 18.0, 1.0); // 10 deg CW tracked rotation
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_FALSE(plan.lines[0].parts.empty());
	std::string const& text = plan.lines[0].parts.front().text;
	// \fr30 is the inline rotation base (30 deg CCW); the 10 deg CW tracked
	// rotation composes to 20 deg CCW. \fr feeds the same base as \frz and
	// drops with the rest of the re-emitted family -- leaving it in the
	// kept bytes would win last-wins over the appended \frz and silently
	// lose the 30 deg base. \b1 is not a planner tag: it stays
	// byte-for-byte and the transform still lands after it.
	size_t const b1 = text.find(R"(\b1)");
	size_t const pos = text.find(R"(\pos(0.00,0.00))");
	size_t const frz = text.find(R"(\frz(20.00))");
	ASSERT_NE(std::string::npos, b1) << text;
	ASSERT_NE(std::string::npos, pos) << text;
	ASSERT_NE(std::string::npos, frz) << text;
	EXPECT_GT(pos, b1);
	EXPECT_GT(frz, b1);
	EXPECT_EQ(std::string::npos, text.find(R"(\fr30)")) << text;

	// The bare \fr alias and \frz feed the same base: byte-equal output.
	Fixture fx2;
	auto *zline = fx2.AddLine(0, 2000, R"({\b1\frz30\pos(0,0)}q)");
	auto zplan = BuildApplyPlan(fx2.file, {zline}, input);
	ASSERT_TRUE(zplan.has_mutations());
	EXPECT_EQ(text, zplan.lines[0].parts.front().text);
}

TEST(motion_track_apply_plan, absolute_coordinate_tags_flag_line_for_review) {
	// \org / \clip / \iclip pin absolute script-space geometry the emitted
	// \pos/\move cannot follow (including \t-animated instances), so the
	// plan lists those lines for the caller's manual-review warning while
	// still applying to them.
	Fixture fx;
	auto *org_line = fx.AddLine(0, 2000, R"({\org(960,540)\pos(10,10)}a)");
	auto *clip_line = fx.AddLine(0, 2000, R"({\clip(0,0,100,100)\pos(10,10)}b)");
	auto *iclip_line = fx.AddLine(0, 2000, R"({\iclip(0,0,100,100)\pos(10,10)}c)");
	auto *animated = fx.AddLine(0, 2000, R"({\t(\org(5,5))\pos(10,10)}d)");
	auto *clean = fx.AddLine(0, 2000, R"({\pos(10,10)}e)");
	auto input = BaseInput();
	input.samples = LinearSamples(0, 29, 0, 0, 0.0, 0.0);

	auto plan = BuildApplyPlan(
		fx.file, {org_line, clip_line, iclip_line, animated, clean}, input);
	ASSERT_TRUE(plan.has_mutations());
	ASSERT_EQ(size_t(4), plan.needs_manual_review.size());
	EXPECT_EQ(org_line, plan.needs_manual_review[0]);
	EXPECT_EQ(clip_line, plan.needs_manual_review[1]);
	EXPECT_EQ(iclip_line, plan.needs_manual_review[2]);
	EXPECT_EQ(animated, plan.needs_manual_review[3]);
	EXPECT_EQ(std::end(plan.needs_manual_review),
			  std::find(plan.needs_manual_review.begin(),
						plan.needs_manual_review.end(), clean));
	// The flagged lines still receive their planned rewrite.
	ASSERT_EQ(size_t(5), plan.lines.size());
}

TEST(motion_track_apply_plan, compact_pieces_stay_within_epsilon_and_continuous) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}curve)");
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
		if (part.covered)
			++covered_parts;
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
	PlannedLinePart const *prev = nullptr;
	for (auto const& part : parts) {
		if (!part.covered)
			continue;
		if (prev)
			EXPECT_EQ(prev->end_ms, part.start_ms);
		prev = &part;
	}
}

TEST(motion_track_apply_plan, smooth_preserves_constant_velocity_exactly) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(50,60)}slide)");
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(50,60)}jitter)");
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
	auto *line = fx.AddLine(0, 2000, R"({\pos(0,0)}track)");
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
		auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}bump)");
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

TEST(motion_track_apply_plan, source_snapshot_rejects_stale_text_and_timecodes) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, "keep");
	auto snap = CaptureMotionTrackSource(fx.file, {line}, fx.fps, 30);
	std::string message;
	auto resolved = ResolveMotionTrackSourceLines(fx.file, snap, message);
	ASSERT_EQ(1u, resolved.size());
	EXPECT_EQ(line, resolved[0]);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(fx.file, snap, fx.fps));

	line->Text = "changed";
	EXPECT_FALSE(MotionTrackSourceIsCurrent(fx.file, snap, fx.fps));
	resolved = ResolveMotionTrackSourceLines(fx.file, snap, message);
	EXPECT_TRUE(resolved.empty());
	EXPECT_FALSE(message.empty());

	line->Text = "keep";
	EXPECT_TRUE(MotionTrackSourceIsCurrent(fx.file, snap, fx.fps));
	EXPECT_FALSE(MotionTrackSourceIsCurrent(fx.file, snap,
											agi::vfr::Framerate(24.0)));
}

TEST(motion_track_apply_plan, continue_targets_require_unchanged_times) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, "keep");
	auto snap = CaptureMotionTrackSource(fx.file, {line}, fx.fps, 30);
	EXPECT_TRUE(MotionTrackContinueTargetsMatch(snap, {line}));
	EXPECT_TRUE(MotionTrackTimecodesMatch(snap, fx.fps));

	line->Start = 100;
	EXPECT_FALSE(MotionTrackContinueTargetsMatch(snap, {line}));
	line->Start = 0;
	line->End = 1500;
	EXPECT_FALSE(MotionTrackContinueTargetsMatch(snap, {line}));
	line->End = 2000;
	EXPECT_TRUE(MotionTrackContinueTargetsMatch(snap, {line}));

	line->Text = "still the same objects and times";
	EXPECT_TRUE(MotionTrackContinueTargetsMatch(snap, {line}));

	EXPECT_TRUE(MotionTrackTimecodesMatch(snap, agi::vfr::Framerate(10.0)));
	EXPECT_FALSE(MotionTrackTimecodesMatch(snap, agi::vfr::Framerate(24.0)));

	// CFR 10 fps and a uniform v2 table with the same frame boundaries are
	// the same consumed timeline; IsVFR must not reject the session.
	std::vector<int> uniform;
	uniform.reserve(31);
	for (int f = 0; f <= 30; ++f)
		uniform.push_back(f * 100);
	agi::vfr::Framerate const v2(uniform);
	ASSERT_TRUE(v2.IsVFR());
	ASSERT_FALSE(fx.fps.IsVFR());
	EXPECT_TRUE(MotionTrackTimecodesMatch(snap, v2));
}

TEST(motion_track_apply_plan, timecode_fingerprint_covers_final_end_boundary) {
	// Two v1 timecode tracks that agree on every frame start of a 7-frame
	// video and on the assumed fps (so FPSFraction matches), but differ in
	// the boundary after the last covered frame: the final range runs at
	// 3 fps vs 30 fps, which only moves the trailing sentinel. Apply reads
	// that boundary as TimeAtFrame(last, END); the fingerprint must see it
	// or a stale session survives on a changed timeline.
	auto write_track = [](char const *final_range_fps) {
		auto path = std::filesystem::temp_directory_path() / ("aegisub_motion_track_tc_" + std::string(final_range_fps) + ".txt");
		std::ofstream out(path, std::ios::binary);
		out << "# timecode format v1\nAssume 30\n0,5,30\n6,6,"
			<< final_range_fps << "\n";
		return path;
	};
	auto const fast = write_track("30");
	auto const slow = write_track("3");
	std::error_code ec;
	auto cleanup = agi::make_scope_exit([&] {
		std::filesystem::remove(fast, ec);
		std::filesystem::remove(slow, ec);
	});

	agi::vfr::Framerate const fast_fps(fast);
	agi::vfr::Framerate const slow_fps(slow);
	ASSERT_EQ(fast_fps.TimeAtFrame(6), slow_fps.TimeAtFrame(6));
	ASSERT_NE(fast_fps.TimeAtFrame(6, agi::vfr::Time::END),
			  slow_fps.TimeAtFrame(6, agi::vfr::Time::END));

	Fixture fx;
	auto *line = fx.AddLine(0, 200, "keep");
	auto snap = CaptureMotionTrackSource(fx.file, {line}, fast_fps, 7);
	EXPECT_TRUE(MotionTrackTimecodesMatch(snap, fast_fps));
	EXPECT_FALSE(MotionTrackTimecodesMatch(snap, slow_fps));
}

TEST(motion_track_apply_plan, disjoint_target_outside_domain_is_not_ok) {
	Fixture fx;
	auto *line = fx.AddLine(0, 50, "early");
	auto input = BaseInput();
	input.decode_interval = FrameInterval{20, 29};
	input.direction_domain = FrameInterval{20, 29};
	input.samples = LinearSamples(20, 29, 0, 0, 1.0, 0.0);
	auto plan = BuildApplyPlan(fx.file, {line}, input);
	EXPECT_EQ(ApplyPlanStatus::Ok, plan.status);
	EXPECT_FALSE(plan.has_mutations());
	EXPECT_TRUE(plan.lines.empty());
}
