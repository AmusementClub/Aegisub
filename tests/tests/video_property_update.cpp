#include <main.h>

#include "../../src/ass_dialogue.h"
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

TEST(video_property_update, missing_layout_res_prompts_even_when_script_resolution_matches_video) {
	VideoPropertyUpdateInput input;
	input.set_properties = true;
	input.resolution_type = ScriptResolutionType::PlayRes;
	input.script_width = 1920;
	input.script_height = 1080;
	input.video_width = 1920;
	input.video_height = 1080;
	input.mismatch_mode = VideoResolutionMismatchMode::Ignore;

	auto plan = PlanVideoPropertyUpdate(input);

	EXPECT_TRUE(plan.prompt_for_layout_res);
	EXPECT_FALSE(plan.prompt_for_resolution_mismatch);
	EXPECT_FALSE(plan.set_resolution);
}

TEST(video_property_update, incomplete_layout_res_prompts_even_when_script_resolution_matches_video) {
	VideoPropertyUpdateInput input;
	input.set_properties = true;
	input.resolution_type = ScriptResolutionType::PlayRes;
	input.script_width = 1920;
	input.script_height = 1080;
	input.video_width = 1920;
	input.video_height = 1080;
	input.layout_res_x = 1920;
	input.layout_res_y = 0;
	input.mismatch_mode = VideoResolutionMismatchMode::Ignore;

	auto plan = PlanVideoPropertyUpdate(input);

	EXPECT_TRUE(plan.prompt_for_layout_res);
	EXPECT_FALSE(plan.prompt_for_resolution_mismatch);
	EXPECT_FALSE(plan.set_resolution);
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

TEST(resolution_resampler, clamps_negative_renderer_clamped_override_values_when_rewriting) {
	AssFile file;
	file.LoadDefault(false);
	file.Events.push_back(*new AssDialogue("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\\bord-2\\xbord-3\\ybord-4\\shad-5\\xshad-6\\yshad-7\\be-8\\blur-9\\fsp-10}x"));

	ResampleSettings settings = {};
	settings.source_x = 640;
	settings.source_y = 480;
	settings.dest_x = 1280;
	settings.dest_y = 960;
	settings.ar_mode = ResampleARMode::Stretch;
	settings.source_matrix = YCbCrMatrix::rgb;
	settings.dest_matrix = YCbCrMatrix::rgb;

	ResampleResolution(&file, settings);

	auto const& text = file.Events.front().Text.get();
	EXPECT_EQ("{\\bord0\\xbord0\\ybord0\\shad0\\xshad-12\\yshad-14\\be0\\blur0\\fsp-20}x", text);
}

TEST(resolution_resampler, rewrites_compatible_override_colors_with_shared_formatter) {
	AssFile file;
	file.LoadDefault(false);
	file.Events.push_back(*new AssDialogue("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\\c&HFFFFFF&tail}x"));

	ResampleSettings settings = {};
	settings.source_x = 640;
	settings.source_y = 480;
	settings.dest_x = 640;
	settings.dest_y = 480;
	settings.ar_mode = ResampleARMode::Stretch;
	settings.source_matrix = YCbCrMatrix::tv_601;
	settings.dest_matrix = YCbCrMatrix::tv_709;

	ResampleResolution(&file, settings);

	auto const& text = file.Events.front().Text.get();
	EXPECT_EQ("{\\c&HFFFFFF&}x", text);
}
