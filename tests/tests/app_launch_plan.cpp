#include <gtest/gtest.h>

#include "../../src/app_launch_plan.h"

#include <variant>

TEST(app_launch_plan, leaves_normal_gui_file_arguments_untouched) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "subtitle.ass"});

	EXPECT_EQ(plan.mode, AppLaunchMode::Gui);
	EXPECT_TRUE(plan.ParseSucceeded());
	EXPECT_FALSE(plan.immediate_exit_code);
}

TEST(app_launch_plan, rejects_removed_cli_entry) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--cli", "inspect", "media"});

	EXPECT_EQ(plan.mode, AppLaunchMode::Headless);
	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_NE(plan.error.find("--cli is no longer supported"), std::string::npos);
	EXPECT_FALSE(plan.headless_service);
}

TEST(app_launch_plan, rejects_removed_playback_probe_entry) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--headless-playback-probe"});

	EXPECT_EQ(plan.mode, AppLaunchMode::Headless);
	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_NE(plan.error.find("--headless-playback-probe is no longer supported"), std::string::npos);
}

TEST(app_launch_plan, headless_help_is_an_immediate_success_with_all_command_families) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--headless", "--help"});

	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 0);
	EXPECT_TRUE(plan.ParseSucceeded());
	EXPECT_NE(plan.immediate_output.find("run"), std::string::npos);
	EXPECT_NE(plan.immediate_output.find("probe"), std::string::npos);
	EXPECT_NE(plan.immediate_output.find("inspect"), std::string::npos);
}

TEST(app_launch_plan, gui_test_help_is_an_immediate_success) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--gui-test", "--help"});

	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 0);
	EXPECT_NE(plan.immediate_output.find("host"), std::string::npos);
	EXPECT_NE(plan.immediate_output.find("run"), std::string::npos);
}

TEST(app_launch_plan, parses_probe_to_typed_service_command) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "probe", "playback",
		"--probe-video", "input.mkv",
		"--probe-audio", "input.flac",
		"--probe-repeat-count", "2",
		"--probe-seek-after-ms", "25",
		"--probe-seek-target-offset-ms", "50",
		"--probe-video-track-index", "1",
		"--probe-audio-rate-scale", "1.25"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.headless_service);
	auto const* command = std::get_if<aegisub::headless_service_cli::ProbePlaybackCommand>(&*plan.headless_service);
	ASSERT_NE(command, nullptr);
	EXPECT_EQ(command->request.video_path.filename(), "input.mkv");
	EXPECT_EQ(command->request.audio_path.filename(), "input.flac");
	EXPECT_EQ(command->request.repeat_count, 2);
	EXPECT_EQ(command->request.seek_after_ms, 25);
	EXPECT_EQ(command->request.seek_target_offset_ms, 50);
	EXPECT_EQ(command->request.video_track_index, 1);
	EXPECT_DOUBLE_EQ(command->request.audio_rate_scale, 1.25);
}

TEST(app_launch_plan, probe_skip_audio_does_not_synthesize_an_audio_path) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "probe", "playback",
		"--probe-video", "input.mkv", "--probe-skip-audio"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	auto const* command = std::get_if<aegisub::headless_service_cli::ProbePlaybackCommand>(&*plan.headless_service);
	ASSERT_NE(command, nullptr);
	EXPECT_TRUE(command->request.skip_audio);
	EXPECT_TRUE(command->request.audio_path.empty());
}

TEST(app_launch_plan, decimal_numeric_options_do_not_use_c_or_bool_bases) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "probe", "playback",
		"--probe-video", "input.mkv", "--probe-repeat-count", "010",
		"--probe-line-start-ms", "+0012", "--probe-video-track-index", "08"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	auto const* command = std::get_if<aegisub::headless_service_cli::ProbePlaybackCommand>(
		&*plan.headless_service);
	ASSERT_NE(command, nullptr);
	EXPECT_EQ(command->request.repeat_count, 10);
	EXPECT_EQ(command->request.line_start_ms, 12);
	EXPECT_EQ(command->request.video_track_index, 8);
}

TEST(app_launch_plan, decimal_numeric_options_reject_hex_and_bool_spellings) {
	for (auto const* value : {"0x10", "0b10", "true", "1,000"}) {
		auto plan = ParseAppLaunchPlan({
			"Aegisub", "--headless", "probe", "playback",
			"--probe-video", "input.mkv", "--probe-repeat-count", value});

		EXPECT_FALSE(plan.ParseSucceeded()) << value;
		ASSERT_TRUE(plan.immediate_exit_code);
		EXPECT_EQ(*plan.immediate_exit_code, 64);
	}
}

TEST(app_launch_plan, string_values_do_not_consume_short_options) {
	auto fontcollector = ParseAppLaunchPlan({
		"Aegisub", "fontcollector", "normalize", "input.ass", "--encoding", "-r"});
	auto headless = ParseAppLaunchPlan({
		"Aegisub", "--headless", "run", "--scenario", "-h"});
	auto gui_test = ParseAppLaunchPlan({
		"Aegisub", "--gui-test", "host", "--profile-dir", "-h"});

	for (auto const* plan : {&fontcollector, &headless, &gui_test}) {
		EXPECT_FALSE(plan->ParseSucceeded());
		ASSERT_TRUE(plan->immediate_exit_code);
		EXPECT_EQ(*plan->immediate_exit_code, 64);
	}
}

TEST(app_launch_plan, skip_audio_rejects_explicit_audio_settings) {
	auto probe = ParseAppLaunchPlan({
		"Aegisub", "--headless", "probe", "playback", "--probe-video", "input.mkv",
		"--probe-skip-audio", "--probe-max-abs-delta-ms", "25"});
	auto inspect = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "media", "--video", "input.mkv",
		"--skip-audio", "--audio-provider", "provider"});
	auto probe_path = ParseAppLaunchPlan({
		"Aegisub", "--headless", "probe", "playback", "--probe-video", "input.mkv",
		"--probe-skip-audio", "--probe-audio", "input.flac"});
	auto inspect_path = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "media", "--video", "input.mkv",
		"--skip-audio", "--audio", "input.flac"});

	EXPECT_FALSE(probe.ParseSucceeded());
	EXPECT_FALSE(inspect.ParseSucceeded());
	EXPECT_FALSE(probe_path.ParseSucceeded());
	EXPECT_FALSE(inspect_path.ParseSucceeded());
}

TEST(app_launch_plan, probe_rejects_non_finite_rate_scale) {
	for (auto const* value : {"nan", "inf", "-inf"}) {
		auto plan = ParseAppLaunchPlan({
			"Aegisub", "--headless", "probe", "playback",
			"--probe-video", "input.mkv", "--probe-audio-rate-scale", value});

		EXPECT_FALSE(plan.ParseSucceeded()) << value;
		ASSERT_TRUE(plan.immediate_exit_code);
		EXPECT_EQ(*plan.immediate_exit_code, 64);
		EXPECT_NE(plan.error.find("finite positive"), std::string::npos);
	}
}

TEST(app_launch_plan, probe_requires_both_seek_options) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "probe", "playback",
		"--probe-video", "input.mkv", "--probe-seek-after-ms", "25"});

	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_NE(plan.error.find("--probe-seek-target-offset-ms"), std::string::npos);
}

TEST(app_launch_plan, parses_inspect_media_to_typed_service_command) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "media",
		"--video", "input.mkv", "--video-track-index", "2", "--skip-audio"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.headless_service);
	auto const* command = std::get_if<aegisub::headless_service_cli::InspectMediaCommand>(&*plan.headless_service);
	ASSERT_NE(command, nullptr);
	EXPECT_EQ(command->request.video_path.filename(), "input.mkv");
	EXPECT_EQ(command->request.video_track_index, 2);
	EXPECT_TRUE(command->request.skip_audio);
	EXPECT_TRUE(command->request.audio_path.empty());
}

TEST(app_launch_plan, inspect_media_rejects_removed_positional_path_compatibility) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--headless", "inspect", "media", "input.mkv"});

	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_FALSE(plan.headless_service);
	EXPECT_NE(plan.error.find("--video"), std::string::npos);
}

TEST(app_launch_plan, inspect_media_does_not_consume_the_next_option_as_a_path) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "media", "--video", "--skip-audio"});

	EXPECT_FALSE(plan.ParseSucceeded());
	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 64);
	EXPECT_FALSE(plan.headless_service);
}

TEST(app_launch_plan, inspect_media_rejects_duplicate_scalar_options) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "media",
		"--video", "one.mkv", "--video", "two.mkv"});

	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_NE(plan.error.find("--video"), std::string::npos);
}

TEST(app_launch_plan, probe_requires_video) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--headless", "probe", "playback"});

	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_FALSE(plan.headless_service);
	EXPECT_NE(plan.error.find("--probe-video"), std::string::npos);
}

TEST(app_launch_plan, inspect_trace_rejects_removed_legacy_option_alias) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "trace", "--trace-dir", "trace"});

	EXPECT_FALSE(plan.ParseSucceeded());
	EXPECT_FALSE(plan.headless_service);
	EXPECT_NE(plan.error.find("--trace-dir"), std::string::npos);
}

TEST(app_launch_plan, inspect_trace_accepts_named_input) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "trace", "--input", "trace"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	auto const* command = std::get_if<aegisub::headless_service_cli::InspectTraceCommand>(&*plan.headless_service);
	ASSERT_NE(command, nullptr);
	EXPECT_EQ(command->request.input_path.filename(), "trace");
}

TEST(app_launch_plan, inspect_ass_info_accepts_path_with_encoding) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "inspect", "ass-info", "input.ass", "--encoding", "UTF-8"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.headless_service);
	auto const* command = std::get_if<aegisub::headless_service_cli::InspectAssInfoCommand>(&*plan.headless_service);
	ASSERT_NE(command, nullptr);
	EXPECT_EQ(command->request.subtitle_path.filename(), "input.ass");
	EXPECT_EQ(command->request.encoding, "UTF-8");
}

TEST(app_launch_plan, rejects_removed_session_grammar) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--headless", "session", "playback"});

	EXPECT_EQ(plan.mode, AppLaunchMode::Headless);
	EXPECT_FALSE(plan.ParseSucceeded());
	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 64);
	EXPECT_NE(plan.error.find("run"), std::string::npos);
}

TEST(app_launch_plan, parses_versioned_headless_run) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "run", "--scenario", "scenario.json",
		"--input", "video=input.mkv", "--input", "subtitle=input.ass", "--keep-profile"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.headless_run);
	EXPECT_FALSE(plan.headless_service);
	EXPECT_EQ(plan.headless_run->scenario_path.filename(), "scenario.json");
	EXPECT_EQ(plan.headless_run->inputs.size(), 2u);
	EXPECT_TRUE(plan.headless_run->keep_profile);
}

TEST(app_launch_plan, headless_run_rejects_an_option_as_the_scenario_value) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--headless", "run", "--scenario", "--keep-profile"});

	EXPECT_FALSE(plan.ParseSucceeded());
	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 64);
}

TEST(app_launch_plan, parses_gui_test_host) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--gui-test", "host", "--open", "one.ass", "--open", "two.ass",
		"--artifacts", "artifacts", "--keep-profile"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	EXPECT_EQ(plan.mode, AppLaunchMode::GuiTest);
	EXPECT_TRUE(plan.gui_test_host);
	ASSERT_TRUE(plan.gui_test_run);
	EXPECT_EQ(plan.gui_test_open_files.size(), 2u);
	EXPECT_TRUE(plan.gui_test_run->keep_profile);
}

TEST(app_launch_plan, parses_gui_test_run_without_internal_worker_flag) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "--gui-test", "run", "--scenario", "scenario.json"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.gui_test_run);
	EXPECT_FALSE(plan.gui_test_host);
	EXPECT_EQ(plan.gui_test_run->scenario_path.filename(), "scenario.json");
	EXPECT_FALSE(plan.gui_test_run->internal_worker);
}

TEST(app_launch_plan, gui_test_requires_an_explicit_subcommand) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "--gui-test"});

	EXPECT_FALSE(plan.ParseSucceeded());
	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 64);
}

TEST(app_launch_plan, fontcollector_help_is_an_immediate_success) {
	auto plan = ParseAppLaunchPlan({"Aegisub", "fontcollector", "--help"});

	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 0);
	EXPECT_TRUE(plan.ParseSucceeded());
	EXPECT_NE(plan.immediate_output.find("check"), std::string::npos);
	EXPECT_NE(plan.immediate_output.find("collect"), std::string::npos);
	EXPECT_NE(plan.immediate_output.find("normalize"), std::string::npos);
}

TEST(app_launch_plan, parses_fontcollector_check_command) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "fontcollector", "check", "one.ass", "two.ass",
		"--matcher", "libass", "--additional-fonts", "fonts", "--details",
		"--json", "--recursive", "--strict"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.fontcollector_options);
	auto const& options = *plan.fontcollector_options;
	EXPECT_EQ(options.operation, aegisub::fontcollector_subcommand::Operation::Check);
	EXPECT_EQ(options.inputs, (std::vector<std::string>{"one.ass", "two.ass"}));
	EXPECT_EQ(options.matcher, "libass");
	EXPECT_EQ(options.additional_fonts, (std::vector<std::string>{"fonts"}));
	EXPECT_TRUE(options.details);
	EXPECT_TRUE(options.json);
	EXPECT_TRUE(options.recursive);
	EXPECT_TRUE(options.strict);
}

TEST(app_launch_plan, fontcollector_repeatable_options_do_not_consume_positional_inputs) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "fontcollector", "check",
		"--additional-fonts", "fonts-one", "--additional-fonts", "fonts-two",
		"input.ass", "--matcher", "libass"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.fontcollector_options);
	EXPECT_EQ(plan.fontcollector_options->inputs, (std::vector<std::string>{"input.ass"}));
	EXPECT_EQ(
		plan.fontcollector_options->additional_fonts,
		(std::vector<std::string>{"fonts-one", "fonts-two"}));
}

TEST(app_launch_plan, parses_fontcollector_collect_destination) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "fontcollector", "collect", "input.ass", "--to", "fonts",
		"--strict"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.fontcollector_options);
	auto const& options = *plan.fontcollector_options;
	EXPECT_EQ(options.operation, aegisub::fontcollector_subcommand::Operation::Collect);
	EXPECT_EQ(options.destination, "fonts");
	EXPECT_FALSE(options.copy_to_script_directory);
	EXPECT_TRUE(options.strict);
}

TEST(app_launch_plan, fontcollector_collect_rejects_two_destinations) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "fontcollector", "collect", "input.ass", "--to", "fonts",
		"--to-script-dir"});

	EXPECT_FALSE(plan.ParseSucceeded());
	ASSERT_TRUE(plan.immediate_exit_code);
	EXPECT_EQ(*plan.immediate_exit_code, 64);
	EXPECT_FALSE(plan.fontcollector_options);
}

TEST(app_launch_plan, parses_fontcollector_normalize_command) {
	auto plan = ParseAppLaunchPlan({
		"Aegisub", "fontcollector", "normalize", "input.ass",
		"--target", "english", "--encoding", "UTF-8", "--json"});

	ASSERT_TRUE(plan.ParseSucceeded()) << plan.error;
	ASSERT_TRUE(plan.fontcollector_options);
	auto const& options = *plan.fontcollector_options;
	EXPECT_EQ(options.operation, aegisub::fontcollector_subcommand::Operation::Normalize);
	EXPECT_EQ(options.normalization_target, "english");
	EXPECT_EQ(options.encoding, "UTF-8");
	EXPECT_TRUE(options.json);
}
