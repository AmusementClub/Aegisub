#include <main.h>

#include "../../src/motion_track/apply_source.h"
#include "../../src/perspective_ass_state.h"

#include <ass_file.h>
#include <ass_style.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;

class motion_track_apply_source : public testing::Test {
	protected:
	AssFile file;
	MotionTrackApplySource source;
	ApplyPlanInput input;
	AssDialogue *untouched = nullptr;
	std::vector<AssDialogue *> selection;
	int commits = 0;

	void SetUp() override {
		file.Styles.push_back(*new AssStyle);
		auto *first = AddLine(R"({\pos(0,0)\frz15\fscx120\fscy80}first)");
		untouched = AddLine("unselected");
		auto *second = AddLine(R"({\pos(20,30)\frz15\fscx120\fscy80}second)");
		file.Commit("setup", AssFile::COMMIT_DIAG_ADDREM);
		selection = {first, second};
		input.model = TrackModel::Similarity;
		input.decode_interval = {.first = 0, .last = 19};
		input.direction_domain = {.first = 0, .last = 19};
		input.storage_width = input.script_width = 1920;
		input.storage_height = input.script_height = 1080;
		input.timecodes = agi::vfr::Framerate(10.0);
		input.video_frame_count = 20;
		input.options.compact_epsilon = 100.0;
		for (int frame = 0; frame < 20; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			sample.center_x = 2.0 * frame;
			double const angle = 0.5 * frame * std::numbers::pi / 180.0;
			double const scale = 1.0 + 0.01 * frame;
			sample.transform.matrix[0] = scale * std::cos(angle);
			sample.transform.matrix[1] = -scale * std::sin(angle);
			sample.transform.matrix[3] = scale * std::sin(angle);
			sample.transform.matrix[4] = scale * std::cos(angle);
			input.samples.push_back(sample);
		}
		source.Capture(file, selection, input.timecodes, input.video_frame_count);
	}

	AssDialogue *AddLine(std::string text) {
		auto *line = new AssDialogue;
		line->Start = 0;
		line->End = 1950;
		line->Text = std::move(text);
		file.Events.push_back(*line);
		return line;
	}

	bool Apply(MotionTrackApplyPlan const& plan, agi::vfr::Framerate const& timecodes) {
		auto commit = [this](std::vector<AssDialogue *> const& lines) {
			// The UI's old selection remains alive through the commit.
			for (auto *line : selection)
				EXPECT_FALSE(line->Text.get().empty());
			file.Commit("apply", AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);
			selection = lines;
			++commits;
		};
		std::string message;
		return source.Apply(file, plan, timecodes, commit, message);
	}

	MotionTrackApplyPlan Plan(ApplyMode mode) {
		input.options.mode = mode;
		return BuildApplyPlan(file, source.Targets(), input);
	}

	[[nodiscard]] std::vector<std::string> Entries() const {
		std::vector<std::string> entries;
		for (auto const& line : file.Events)
			entries.push_back(line.GetEntryData());
		return entries;
	}
};
} // namespace

TEST_F(motion_track_apply_source, compact_exact_round_trip_reuses_baseline_and_preserves_other_lines) {
	auto compact = Plan(ApplyMode::Compact);
	ASSERT_TRUE(compact.has_mutations()) << compact.message;
	ASSERT_EQ(2u, compact.event_count);
	ASSERT_TRUE(Apply(compact, input.timecodes));
	auto const compact_entries = Entries();
	ASSERT_EQ(3u, compact_entries.size());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));

	auto exact = Plan(ApplyMode::Exact);
	ASSERT_TRUE(exact.has_mutations()) << exact.message;
	ASSERT_EQ(40u, exact.event_count);
	ASSERT_TRUE(Apply(exact, input.timecodes));
	ASSERT_EQ(40u, selection.size());
	EXPECT_EQ(41u, Entries().size());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_EQ(0, selection.front()->Row);
	EXPECT_EQ(40, selection.back()->Row);
	for (int frame : {0, 9, 19}) {
		auto *line = selection[frame];
		auto const state = perspective::EvaluateEffectiveAssState({.file = &file,
																   .line = line,
																   .play_resolution = {.width = 1920.0, .height = 1080.0},
																   .capture_time_ms = input.timecodes.TimeAtFrame(frame)});
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		EXPECT_NEAR(2.0 * frame, state.value.transform.position.x, 1e-6);
		EXPECT_NEAR(15.0 - 0.5 * frame, state.value.transform.rotation_z, 1e-6);
		EXPECT_NEAR(120.0 * (1.0 + 0.01 * frame), state.value.transform.scale_x, 1e-6);
		EXPECT_NEAR(80.0 * (1.0 + 0.01 * frame), state.value.transform.scale_y, 1e-6);
	}
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	EXPECT_EQ(compact_entries, Entries());
	EXPECT_EQ(2u, selection.size());
	EXPECT_EQ(3, commits);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
}

TEST_F(motion_track_apply_source, hand_edit_after_plan_preparation_rejects_reapply_without_mutation) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	selection.front()->Text = "edited after preview";
	auto const edited_entries = Entries();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	EXPECT_EQ(edited_entries, Entries());
	EXPECT_EQ(1, commits);
}

TEST_F(motion_track_apply_source, undo_replacement_rejects_reapply_without_mutation) {
	AssFile original(file);
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	file = original;
	selection.clear();
	auto const restored_entries = Entries();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	EXPECT_EQ(restored_entries, Entries());
	EXPECT_EQ(1, commits);
}

TEST_F(motion_track_apply_source, cleared_source_rejects_pending_plan_without_accessing_old_baseline) {
	auto plan = Plan(ApplyMode::Exact);
	ASSERT_TRUE(plan.has_mutations());
	auto const entries = Entries();
	source.Clear();
	EXPECT_FALSE(Apply(plan, input.timecodes));
	EXPECT_EQ(entries, Entries());
	EXPECT_EQ(0, commits);
}

TEST_F(motion_track_apply_source, changed_style_or_timecodes_rejects_reapply) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	auto const entries = Entries();
	auto *style = file.GetStyle("Default");
	ASSERT_NE(nullptr, style);
	double const previous_scale = style->scalex;
	style->scalex = previous_scale + 10;
	style->UpdateData();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	style->scalex = previous_scale;
	style->UpdateData();
	EXPECT_FALSE(Apply(exact, agi::vfr::Framerate(20.0)));
	EXPECT_EQ(entries, Entries());
	EXPECT_EQ(1, commits);
}
