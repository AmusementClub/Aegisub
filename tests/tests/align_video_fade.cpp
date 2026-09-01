#include <main.h>

#include "../../src/align_video_fade.h"

#include <libaegisub/option.h>
#include <libaegisub/option_value.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

bool Contains(std::string const& text, std::string const& value) {
	return text.find(value) != std::string::npos;
}

} // namespace

TEST(align_video_fade, fits_a_gradual_visibility_ramp) {
	std::array<double, 14> samples = {
		0.00, 0.01, 0.00, 0.02,
		0.12, 0.30, 0.52, 0.73, 0.91, 1.00,
		1.00, 0.99, 1.01, 1.00
	};

	auto const fit = aegisub::align_video_fade::FitVisibilityCurve(samples, 4);
	ASSERT_TRUE(fit.detected);
	EXPECT_GE(fit.outer_index, 2);
	EXPECT_LE(fit.outer_index, 5);
	EXPECT_GE(fit.inner_index, 7);
	EXPECT_LE(fit.inner_index, 10);
	EXPECT_GT(fit.confidence, 0.2);
}

TEST(align_video_fade, reports_first_visible_frame_and_confirmed_plateau) {
	std::array<double, 27> samples = {
		0.00, 0.00, 0.00, 0.00,
		0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45,
		0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.95,
		1.00, 1.00, 1.00, 1.00
	};

	auto const fit = aegisub::align_video_fade::FitVisibilityCurve(samples, 4);
	ASSERT_TRUE(fit.detected);
	EXPECT_EQ(4, fit.outer_index);
	EXPECT_EQ(23, fit.inner_index);
}

TEST(align_video_fade, confirms_the_first_frame_matching_a_full_platform) {
	std::array<double, 8> samples = {
		0.95, 0.97, 0.98, 0.99, 1.00, 1.00, 1.00, 1.00
	};
	std::array<double, 4> platform = { 1.00, 1.00, 1.00, 1.00 };

	EXPECT_EQ(
		4,
		aegisub::align_video_fade::FindConfirmedPlateauStart(samples, platform, 4));
}

TEST(align_video_fade, derives_plateau_tolerance_from_temporal_noise) {
	std::array<double, 8> samples = {
		0.90, 0.94, 0.96, 0.98, 1.01, 0.99, 1.02, 0.98
	};
	std::array<double, 4> platform = { 1.01, 0.99, 1.02, 0.98 };

	EXPECT_EQ(
		2,
		aegisub::align_video_fade::FindConfirmedPlateauStart(samples, platform, 4));
}

TEST(align_video_fade, rejects_a_hard_cut) {
	std::array<double, 12> samples = {
		0.00, 0.01, 0.00, 0.01, 0.00, 0.00,
		1.00, 1.00, 1.00, 1.00, 1.00, 1.00
	};

	auto const fit = aegisub::align_video_fade::FitVisibilityCurve(samples, 4);
	EXPECT_FALSE(fit.detected);
}

TEST(align_video_fade, projects_fade_controls_to_exact_frame_samples) {
	auto const timecodes = agi::vfr::Framerate(24000, 1001);
	int const first_visible = 6;
	int const first_full = 25;
	int const last_full = 45;
	int const last_visible = 64;

	auto const timing = aegisub::align_video_fade::BuildAssFadeTiming(
		timecodes,
		first_visible,
		last_visible,
		first_full,
		last_full,
		true,
		true);

	int const first_sample_ms = timecodes.TimeAtFrame(first_visible, agi::vfr::EXACT);
	int const first_full_sample_ms = timecodes.TimeAtFrame(first_full, agi::vfr::EXACT);
	int const last_full_sample_ms = timecodes.TimeAtFrame(last_full, agi::vfr::EXACT);
	int const last_sample_ms = timecodes.TimeAtFrame(last_visible, agi::vfr::EXACT);
	int const duration_ms = timing.end_ms - timing.start_ms;

	EXPECT_LT(timing.start_ms, first_sample_ms);
	EXPECT_GT(first_sample_ms - timing.start_ms, 0);
	EXPECT_EQ(first_full_sample_ms - timing.start_ms, timing.fade_in_ms);
	EXPECT_EQ(last_full_sample_ms - timing.start_ms, duration_ms - timing.fade_out_ms);
	EXPECT_LT(last_sample_ms, timing.end_ms);
	EXPECT_GT(timing.end_ms - last_sample_ms, 0);
}

TEST(align_video_fade, rounds_absolute_fade_control_points_to_centiseconds) {
	auto const timing = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds({
		101,
		202,
		24,
		26
	});

	// S/P/Q/E are 101/125/176/202 ms and round to 100/130/180/200 ms.
	EXPECT_EQ(100, timing.start_ms);
	EXPECT_EQ(200, timing.end_ms);
	EXPECT_EQ(30, timing.fade_in_ms);
	EXPECT_EQ(20, timing.fade_out_ms);
}

TEST(align_video_fade, rounds_half_centisecond_ties_toward_later_time) {
	auto const timing = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds({
		105,
		205,
		20,
		20
	});

	EXPECT_EQ(110, timing.start_ms);
	EXPECT_EQ(210, timing.end_ms);
	EXPECT_EQ(20, timing.fade_in_ms);
	EXPECT_EQ(20, timing.fade_out_ms);
}

TEST(align_video_fade, rounds_values_on_each_side_of_half_centisecond) {
	auto const before = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds(
		{ 0, 1000, 24, 0 });
	auto const tie = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds(
		{ 0, 1000, 25, 0 });
	auto const after = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds(
		{ 0, 1000, 26, 0 });

	EXPECT_EQ(20, before.fade_in_ms);
	EXPECT_EQ(30, tie.fade_in_ms);
	EXPECT_EQ(30, after.fade_in_ms);
}

TEST(align_video_fade, normalizes_invalid_timing_before_rounding) {
	auto const timing = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds({
		-7,
		34,
		-3,
		100
	});

	EXPECT_EQ(0, timing.start_ms);
	EXPECT_EQ(30, timing.end_ms);
	EXPECT_EQ(0, timing.fade_in_ms);
	EXPECT_EQ(30, timing.fade_out_ms);
}

TEST(align_video_fade, preserves_order_when_fades_would_overlap) {
	auto const timing = aegisub::align_video_fade::RoundAssFadeTimingToCentiseconds({
		100,
		149,
		40,
		40
	});

	EXPECT_EQ(100, timing.start_ms);
	EXPECT_EQ(150, timing.end_ms);
	EXPECT_EQ(40, timing.fade_in_ms);
	EXPECT_EQ(10, timing.fade_out_ms);
	EXPECT_LE(timing.fade_in_ms + timing.fade_out_ms, timing.end_ms - timing.start_ms);
}

TEST(align_video_fade, inserts_fad_for_an_unstyled_line) {
	auto const update = aegisub::align_video_fade::ApplyAssFade("hello", 1000, 200, 300);
	EXPECT_EQ(aegisub::align_video_fade::AssFadeEncoding::Fad, update.encoding);
	EXPECT_EQ("{\\fad(200,300)}hello", update.text);
}

TEST(align_video_fade, updates_existing_fad_and_removes_duplicates) {
	auto const update = aegisub::align_video_fade::ApplyAssFade(
		"{\\fad(10,20)\\fad(30,40)\\fs20}hello", 1000, 200, 300);
	EXPECT_EQ(aegisub::align_video_fade::AssFadeEncoding::Fad, update.encoding);
	EXPECT_EQ(update.text.find("\\fad("), update.text.rfind("\\fad("));
	EXPECT_TRUE(Contains(update.text, "\\fad(200,300)"));
	EXPECT_TRUE(Contains(update.text, "\\fs20"));
}

TEST(align_video_fade, preserves_fade_encoding) {
	auto const update = aegisub::align_video_fade::ApplyAssFade(
		"{\\fade(255,0,255,0,100,500,800)\\bord2}hello", 1000, 200, 300);
	EXPECT_EQ(aegisub::align_video_fade::AssFadeEncoding::Fade, update.encoding);
	EXPECT_TRUE(Contains(update.text, "\\fade(255,0,255,0,200,700,1000)"));
	EXPECT_TRUE(Contains(update.text, "\\bord2"));
	EXPECT_FALSE(Contains(update.text, "\\fad("));
}

TEST(align_video_fade, updates_common_alpha_transform_without_losing_other_transforms) {
	auto const update = aegisub::align_video_fade::ApplyAssFade(
		"{\\alpha&H80&\\t(0,400,\\alpha&H00&)\\t(0,1000,\\frz5)}hello",
		1000,
		200,
		300);
	EXPECT_EQ(aegisub::align_video_fade::AssFadeEncoding::AlphaTransform, update.encoding);
	EXPECT_TRUE(Contains(update.text, "\\alpha&HFF&"));
	EXPECT_TRUE(Contains(update.text, "\\t(0,200,\\alpha&H00&)"));
	EXPECT_TRUE(Contains(update.text, "\\t(700,1000,\\alpha&HFF&)"));
	EXPECT_TRUE(Contains(update.text, "\\t(0,1000,\\frz5)"));
}

TEST(align_video_fade, keeps_alpha_in_later_override_blocks) {
	auto const update = aegisub::align_video_fade::ApplyAssFade(
		"{\\alpha&HFF&\\t(0,400,\\alpha&H00&)}a{\\alpha&H80&}b",
		1000,
		200,
		0);
	EXPECT_EQ(aegisub::align_video_fade::AssFadeEncoding::AlphaTransform, update.encoding);
	EXPECT_TRUE(Contains(update.text, "{\\alpha&H80&}"));
}

TEST(align_video_fade, clamps_fade_durations_to_line_duration) {
	auto const update = aegisub::align_video_fade::ApplyAssFade("x", 100, 80, 80);
	EXPECT_TRUE(Contains(update.text, "\\fad(80,20)"));
}

TEST(align_video_fade, default_configs_disable_fade_time_rounding) {
	for (auto const& relative_path : {
		std::filesystem::path("src/libresrc/default_config.json"),
		std::filesystem::path("src/libresrc/osx/default_config.json"),
	}) {
		std::ifstream stream(std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / relative_path, std::ios::binary);
		ASSERT_TRUE(stream) << relative_path;
		std::string const defaults(std::istreambuf_iterator<char>(stream), {});
		agi::Options options("", {defaults.data(), defaults.size()}, agi::Options::FLUSH_SKIP);

		EXPECT_FALSE(options.Get("Tool/Align to Video/Round Fade Times")->GetBool()) << relative_path;
	}
}
