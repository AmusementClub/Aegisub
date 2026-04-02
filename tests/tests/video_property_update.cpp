#include <main.h>

#include "../../src/ass_file.h"
#include "../../src/resolution_resampler.h"
#include "../../src/video_property_update.h"

TEST(video_property_update, unset_script_resolution_is_forced_to_video_resolution_even_for_dummy_provider) {
	VideoPropertyUpdateInput input;
	input.set_properties = false;
	input.resolution_type = ScriptResolutionType::None;
	input.video_width = 1920;
	input.video_height = 1080;

	auto plan = PlanVideoPropertyUpdate(input);

	EXPECT_TRUE(plan.set_resolution);
	EXPECT_FALSE(plan.prompt_for_resolution_mismatch);
	EXPECT_TRUE(plan.ShouldCommit());
}

TEST(video_property_update, matrix_only_change_keeps_commit_without_prompt) {
	VideoPropertyUpdateInput input;
	input.set_properties = true;
	input.current_matrix = "TV.601";
	input.provider_matrix = "TV.709";
	input.resolution_type = ScriptResolutionType::PlayRes;
	input.script_width = 1280;
	input.script_height = 720;
	input.video_width = 640;
	input.video_height = 360;
	input.mismatch_mode = VideoResolutionMismatchMode::Ignore;

	auto plan = PlanVideoPropertyUpdate(input);

	EXPECT_TRUE(plan.update_matrix);
	EXPECT_FALSE(plan.set_resolution);
	EXPECT_FALSE(plan.resample_mode.has_value());
	EXPECT_FALSE(plan.prompt_for_resolution_mismatch);
	EXPECT_TRUE(plan.ShouldCommit());
}

TEST(video_property_update, set_mode_updates_resolution_without_prompt) {
	VideoPropertyUpdateInput input;
	input.set_properties = true;
	input.resolution_type = ScriptResolutionType::PlayRes;
	input.script_width = 640;
	input.script_height = 360;
	input.video_width = 1280;
	input.video_height = 720;
	input.mismatch_mode = VideoResolutionMismatchMode::Set;

	auto plan = PlanVideoPropertyUpdate(input);

	EXPECT_TRUE(plan.set_resolution);
	EXPECT_FALSE(plan.prompt_for_resolution_mismatch);
	EXPECT_TRUE(plan.ShouldCommit());
}

TEST(video_property_update, resample_mode_auto_stretches_when_aspect_ratio_matches) {
	VideoPropertyUpdateInput input;
	input.set_properties = true;
	input.resolution_type = ScriptResolutionType::PlayRes;
	input.script_width = 640;
	input.script_height = 360;
	input.video_width = 1280;
	input.video_height = 720;
	input.mismatch_mode = VideoResolutionMismatchMode::Resample;

	auto plan = PlanVideoPropertyUpdate(input);

	ASSERT_TRUE(plan.resample_mode.has_value());
	EXPECT_EQ(ResampleARMode::Stretch, *plan.resample_mode);
	EXPECT_FALSE(plan.prompt_for_resolution_mismatch);
}

TEST(video_property_update, resample_mode_prompts_when_aspect_ratio_changes) {
	VideoPropertyUpdateInput input;
	input.set_properties = true;
	input.resolution_type = ScriptResolutionType::PlayRes;
	input.script_width = 640;
	input.script_height = 480;
	input.video_width = 1280;
	input.video_height = 720;
	input.mismatch_mode = VideoResolutionMismatchMode::Resample;

	auto plan = PlanVideoPropertyUpdate(input);

	EXPECT_TRUE(plan.prompt_for_resolution_mismatch);
	EXPECT_TRUE(plan.aspect_ratio_changed);
	EXPECT_FALSE(plan.resample_mode.has_value());
}

TEST(video_property_update, prompt_choice_mapping_rejects_border_actions_when_aspect_ratio_matches) {
	EXPECT_EQ(VideoResolutionMismatchChoice::SetScriptResolution, *ParseVideoResolutionMismatchChoice(0, false));
	EXPECT_EQ(VideoResolutionMismatchChoice::ResampleStretch, *ParseVideoResolutionMismatchChoice(1, false));
	EXPECT_EQ(std::nullopt, ParseVideoResolutionMismatchChoice(2, false));
	EXPECT_EQ(std::nullopt, ParseVideoResolutionMismatchChoice(3, false));
}

TEST(video_property_update, resolve_choice_sets_expected_resample_mode) {
	VideoPropertyUpdatePlan plan;
	plan.prompt_for_resolution_mismatch = true;

	auto resolved = ResolveVideoResolutionMismatchChoice(plan, VideoResolutionMismatchChoice::ResampleRemoveBorder);

	EXPECT_FALSE(resolved.prompt_for_resolution_mismatch);
	ASSERT_TRUE(resolved.resample_mode.has_value());
	EXPECT_EQ(ResampleARMode::RemoveBorder, *resolved.resample_mode);
	EXPECT_FALSE(resolved.set_resolution);
}
