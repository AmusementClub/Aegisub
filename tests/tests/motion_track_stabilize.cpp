// Trajectory stabilization chain and border/shadow/blur growth compensation
// in the apply planner. Both mechanisms are adapted from croni1012/Aegisub
// (src/typesetting_auto_motion.cpp StabilisedPlanar and
// src/typesetting_motion.cpp growth; ISC license); the wiring and assertions
// are ours.

#include <main.h>

#include "../../src/motion_track/apply_plan.h"
#include "../../src/motion_track/types.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>

#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;

constexpr double kPi = std::numbers::pi;

struct Fixture {
	AssFile file;
	std::vector<AssDialogue *> lines;

	Fixture() {
		auto *style = new AssStyle; // Default: outline_w 2, shadow_w 2
		style->name = "Default";
		file.Styles.push_back(*style);
	}

	AssDialogue *AddLine(int start_ms, int end_ms, std::string text) {
		auto *line = new AssDialogue;
		line->Start = start_ms;
		line->End = end_ms;
		line->Text = std::move(text);
		file.Events.push_back(*line);
		lines.push_back(line);
		return line;
	}
};

ApplyPlanInput BaseInput() {
	ApplyPlanInput in;
	in.model = TrackModel::Similarity;
	in.decode_interval = FrameInterval{0, 19};
	in.direction_domain = FrameInterval{0, 19};
	in.storage_width = 1920;
	in.storage_height = 1080;
	in.script_width = 1920;
	in.script_height = 1080;
	in.timecodes = agi::vfr::Framerate(10.0);
	in.video_frame_count = 20;
	in.seed_time_ms = 0;
	in.options.mode = ApplyMode::Exact;
	return in;
}

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

std::string FirstPartText(MotionTrackApplyPlan const& plan) {
	EXPECT_FALSE(plan.lines.empty());
	EXPECT_FALSE(plan.lines[0].parts.empty());
	return plan.lines[0].parts.front().text;
}

// First "\\tag(value" occurrence of any tag in the family list.
bool FindTagValue(std::string const& text, std::string const& tag,
				  double& out) {
	std::string const needle = "\\" + tag + "(";
	auto const at = text.find(needle);
	if (at == std::string::npos)
		return false;
	out = std::strtod(text.c_str() + at + needle.size(), nullptr);
	return true;
}

// Covered parts only: the planner keeps a trailing covered=false suffix
// part whenever the last frame's END time lands before the line end.
size_t CoveredParts(MotionTrackApplyPlan const& plan) {
	size_t n = 0;
	for (auto const& p : plan.lines[0].parts)
		if (p.covered)
			++n;
	return n;
}

std::vector<double> TagValues(MotionTrackApplyPlan const& plan,
							  std::string const& tag) {
	std::vector<double> values;
	for (auto const& part : plan.lines[0].parts) {
		double v = 0.0;
		if (FindTagValue(part.text, tag, v))
			values.push_back(v);
	}
	return values;
}

} // namespace

TEST(motion_track_stabilize, flattens_pose_noise_below_the_floor) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		// Frame 0 is the exact seed anchor; later frames carry sub-floor
		// sensor noise (angle < 0.2 deg, scale < 0.4%, positions constant).
		double const rot_jit = f == 0 ? 0.0 : 0.20 * std::sin(f * 0.7);
		double const scale_jit = f == 0 ? 0.0 : 0.003 * std::sin(f * 1.1);
		FillPose(s, 100.0, 100.0, -rot_jit * kPi / 180.0,
				 1.0 + scale_jit);
		samples.push_back(s);
	}
	input.samples = samples;

	input.options.stabilization.enable = true;
	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	// Everything flattens to the seed pose, so Exact merges one covered
	// part whose tags collapse to the bare style-equivalent position.
	ASSERT_EQ(1u, CoveredParts(plan));
	EXPECT_EQ(R"({\pos(100.00,100.00)}x)",
			  plan.lines[0].parts.front().text);

	input.options.stabilization.enable = false;
	auto raw = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, raw.status);
	// Without the chain the jitter survives rounding and splits the run.
	EXPECT_GT(CoveredParts(raw), 1u);
}

TEST(motion_track_stabilize, unwaps_angle_across_the_wrap_boundary) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		// A continuous 170..189 deg rotation. The planner's atan2 resolves
		// it into (-180, 180]: past 180 the raw emission wraps to -179..,
		// the sequence the unwrap chain repairs.
		FillPose(s, 100.0, 100.0, -(170.0 + f) * kPi / 180.0, 1.0);
		samples.push_back(s);
	}
	input.samples = samples;

	input.options.stabilization.enable = true;
	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const frz = TagValues(plan, "frz");
	ASSERT_EQ(20u, frz.size());
	for (size_t i = 1; i < frz.size(); ++i)
		EXPECT_NEAR(1.0, frz[i] - frz[i - 1], 0.011) << "index " << i;
	EXPECT_GT(frz.back(), 180.5); // crossed 180 instead of wrapping to -179

	input.options.stabilization.enable = false;
	auto raw = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, raw.status);
	auto const raw_frz = TagValues(raw, "frz");
	ASSERT_EQ(20u, raw_frz.size());
	bool saw_wrap = false;
	for (size_t i = 1; i < raw_frz.size(); ++i)
		if (raw_frz[i] < raw_frz[i - 1] - 100.0)
			saw_wrap = true;
	EXPECT_TRUE(saw_wrap); // 180 -> -179 raw, the wrap the unwrap repairs
}

TEST(motion_track_stabilize, flattens_position_jitter_below_the_floor) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	input.model = TrackModel::Translation;
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.samples.clear();
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		// Sub-floor jitter (storage == script pixels here): |dx| < 0.14,
		// |dy| < 0.12, both below the 0.35 px floor, seed frame exact.
		s.center_x = 100.0 + (f == 0 ? 0.0 : 0.14 * std::sin(f * 1.7));
		s.center_y = 100.0 + (f == 0 ? 0.0 : 0.12 * std::cos(f * 0.9));
		input.samples.push_back(s);
	}

	input.options.stabilization.enable = true;
	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(1u, CoveredParts(plan));
	EXPECT_NE(std::string::npos,
			  plan.lines[0].parts.front().text.find(R"(\pos(100.00,100.00))"));
}

TEST(motion_track_stabilize, growth_scales_bord_shad_blur) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)\blur3}x)");
	auto input = BaseInput();
	// Growth compensation is opt-in (default off) since the dialog
	// checkbox landed; these tests exercise it enabled.
	input.options.scale_border = true;
	input.options.scale_shadow = true;
	input.options.scale_blur = true;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 2.0);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const text = FirstPartText(plan);
	// Style defaults outline 2 / shadow 2, inline \blur3; a uniform x2 zoom
	// doubles each (geometric mean of the per-axis growths).
	EXPECT_NE(std::string::npos, text.find(R"(\bord(4.00))"));
	EXPECT_NE(std::string::npos, text.find(R"(\shad(4.00))"));
	EXPECT_NE(std::string::npos, text.find(R"(\blur(6.00))"));
}

TEST(motion_track_stabilize, growth_flags_disable_emission) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)\blur3}x)");
	auto input = BaseInput();
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	input.options.scale_border = false;
	input.options.scale_shadow = false;
	input.options.scale_blur = false;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 2.0);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const text = FirstPartText(plan);
	EXPECT_EQ(std::string::npos, text.find("\\bord("));
	EXPECT_EQ(std::string::npos, text.find("\\shad("));
	EXPECT_EQ(std::string::npos, text.find("\\blur("));
}

TEST(motion_track_stabilize, identity_growth_emits_nothing) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	// Growth compensation is opt-in (default off) since the dialog
	// checkbox landed; these tests exercise it enabled.
	input.options.scale_border = true;
	input.options.scale_shadow = true;
	input.options.scale_blur = true;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 1.0);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const text = FirstPartText(plan);
	EXPECT_EQ(std::string::npos, text.find("\\bord("));
	EXPECT_EQ(std::string::npos, text.find("\\shad("));
	EXPECT_EQ(std::string::npos, text.find("\\blur("));
}

TEST(motion_track_stabilize, growth_uses_per_axis_bord_tags) {
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)\xbord3\ybord2}x)");
	auto input = BaseInput();
	// Growth compensation is opt-in (default off) since the dialog
	// checkbox landed; these tests exercise it enabled.
	input.options.scale_border = true;
	input.options.scale_shadow = true;
	input.options.scale_blur = true;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 2.0);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const text = FirstPartText(plan);
	EXPECT_NE(std::string::npos, text.find(R"(\xbord(6.00))"));
	EXPECT_NE(std::string::npos, text.find(R"(\ybord(4.00))"));
	EXPECT_EQ(std::string::npos, text.find("\\bord("));
}

TEST(motion_track_stabilize, growth_replaces_inline_rounding_to_style_default) {
	// Style outline 2 with an inline \bord4 at half scale: the compensated
	// value rounds back to the style default, but without re-emission the
	// inline override would keep rendering 4. The paren-less short form is
	// not strippable by name, so correctness is the emitted \bord landing
	// after it in the same override block (later tags win).
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)\bord4}x)");
	auto input = BaseInput();
	// Growth compensation is opt-in (default off) since the dialog
	// checkbox landed; these tests exercise it enabled.
	input.options.scale_border = true;
	input.options.scale_shadow = true;
	input.options.scale_blur = true;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 0.5);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const text = FirstPartText(plan);
	auto const emitted = text.find(R"(\bord(2.00))");
	ASSERT_NE(std::string::npos, emitted);
	if (auto const stale = text.find("\\bord4"); stale != std::string::npos)
		EXPECT_LT(stale, emitted);
}

TEST(motion_track_stabilize, growth_reemits_inline_at_identity_transform) {
	// The inline override wins over the style until something replaces it,
	// so even an identity transform re-emits the composed value explicitly
	// instead of leaving the stale override as the only \bord in force.
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)\bord4}x)");
	auto input = BaseInput();
	// Growth compensation is opt-in (default off) since the dialog
	// checkbox landed; these tests exercise it enabled.
	input.options.scale_border = true;
	input.options.scale_shadow = true;
	input.options.scale_blur = true;
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 1.0);
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	auto const text = FirstPartText(plan);
	auto const emitted = text.find(R"(\bord(4.00))");
	ASSERT_NE(std::string::npos, emitted);
	if (auto const stale = text.find("\\bord4"); stale != std::string::npos)
		EXPECT_LT(stale, emitted);
}

TEST(motion_track_stabilize, exact_mode_merges_identical_parts_across_held_gap) {
	// A static pose tracked around an interior Failed gap: the gap frames
	// hold the previous Ok pose, which is byte-identical to the tracked
	// pose on both sides. The grouping never crosses the hold boundary,
	// but the output-side merge must fold the tracked/held/tracked triplet
	// of identical texts into ONE event that renders the same at every
	// frame time (adapted from croni1012/Aegisub's adjacent identical
	// output merge).
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, 100.0, 100.0, 0.0, 1.0);
		if (f >= 8 && f <= 10)
			s.status = TrackStatus::Failed;
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	// 20 identical adjacent frames -> exactly one covered part and one
	// emitted event (the vfr-END sliver after the domain stays uncovered).
	EXPECT_EQ(1u, CoveredParts(plan));
	EXPECT_EQ(1u, plan.event_count);
	auto const& parts = plan.lines[0].parts;
	ASSERT_FALSE(parts.empty());
	EXPECT_TRUE(parts.front().covered);
	EXPECT_EQ(R"({\pos(100.00,100.00)}x)", parts.front().text);
	// The merged part spans the whole covered domain from the line start...
	EXPECT_EQ(0, parts.front().start_ms);
	// ...and the timeline still tiles without overlap or gaps.
	int prev_end = parts.front().start_ms;
	for (auto const& p : parts) {
		EXPECT_EQ(prev_end, p.start_ms);
		prev_end = p.end_ms;
	}
	EXPECT_EQ(2000, prev_end);
}

TEST(motion_track_stabilize, exact_mode_gap_merge_still_splits_on_pose_change) {
	// Same gap layout, but the tracked pose jumps after it: the held
	// stretch is identical to the run before the gap (one merged event)
	// while the post-gap pose stays its own event.
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	input.origin_center_x = 100;
	input.origin_center_y = 100;

	std::vector<TrackSample> samples;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		FillPose(s, f <= 10 ? 100.0 : 110.0, 100.0, 0.0, 1.0);
		if (f >= 8 && f <= 10)
			s.status = TrackStatus::Failed;
		samples.push_back(s);
	}
	input.samples = samples;

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	EXPECT_EQ(2u, CoveredParts(plan));
	EXPECT_EQ(2u, plan.event_count);
	// Frames 8..10 hold frame 7's (100,100) pose and fold into the pre-gap
	// event, which therefore owns the shared knot and ends where the
	// (110,100) stretch begins (frame 11's start).
	int const split = input.timecodes.TimeAtFrame(11, agi::vfr::Time::START);
	bool found_before = false, found_after = false;
	for (auto const& p : plan.lines[0].parts) {
		if (!p.covered)
			continue;
		found_before = found_before || (p.text.find(R"(\pos(100.00,100.00))") != std::string::npos && p.end_ms == split);
		found_after = found_after || (p.text.find(R"(\pos(110.00,100.00))") != std::string::npos && p.start_ms == split);
	}
	EXPECT_TRUE(found_before);
	EXPECT_TRUE(found_after);
}

TEST(motion_track_stabilize, exact_mode_identical_poses_split_when_not_adjacent) {
	// Negative control: a genuinely different pose on the middle frame
	// still splits the run, and the identical poses around it stay
	// SEPARATE events -- only adjacent identical parts merge, never across
	// a different one. Translation model, so the parts carry \pos only.
	Fixture fx;
	auto *line = fx.AddLine(0, 2000, R"({\pos(100,100)}x)");
	auto input = BaseInput();
	input.model = TrackModel::Translation;
	input.origin_center_x = 100;
	input.origin_center_y = 100;
	for (int f = 0; f <= 19; ++f) {
		TrackSample s;
		s.frame = f;
		s.status = TrackStatus::Ok;
		s.center_x = f == 10 ? 130.0 : 100.0;
		s.center_y = 100.0;
		input.samples.push_back(s);
	}

	auto plan = BuildApplyPlan(fx.file, {line}, input);
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	EXPECT_EQ(3u, CoveredParts(plan));
	EXPECT_EQ(3u, plan.event_count);
	size_t identical = 0, bump = 0;
	for (auto const& p : plan.lines[0].parts) {
		if (!p.covered)
			continue;
		if (p.text.find(R"(\pos(100.00,100.00))") != std::string::npos)
			++identical;
		if (p.text.find(R"(\pos(130.00,100.00))") != std::string::npos)
			++bump;
	}
	EXPECT_EQ(2u, identical); // same pose, two non-adjacent events
	EXPECT_EQ(1u, bump);
}
