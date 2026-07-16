#include "../../src/skia/audio/skia_audio_frame_model.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace audio = aegisub::skia::audio;

TEST(skia_audio_frame_model, zoom_math_matches_legacy_audio_display) {
	EXPECT_EQ(audio::AudioZoomFactor(0), 100);
	EXPECT_EQ(audio::AudioZoomFactor(1), 125);
	EXPECT_EQ(audio::AudioZoomFactor(-5), 50);
	EXPECT_EQ(audio::AudioZoomFactor(-11), 20);
	EXPECT_EQ(audio::AudioZoomFactor(-30), 1);
	EXPECT_DOUBLE_EQ(audio::AudioMillisecondsPerLogicalPixel(0), 20.0);
	EXPECT_DOUBLE_EQ(audio::AudioMillisecondsPerLogicalPixel(1), 16.0);
}

TEST(skia_audio_frame_model, viewport_preserves_fractional_dpi_scroll_alignment) {
	audio::FrameViewportRequest request;
	request.logical_width = 801;
	request.logical_height = 200;
	request.content_scale = 1.25;
	request.timeline_height = 18;
	request.scrollbar_height = 15;
	request.scroll_left = 3;
	request.duration_ms = 60000;
	request.milliseconds_per_logical_pixel = 20.0;

	auto const viewport = audio::BuildFrameViewport(request);
	ASSERT_TRUE(viewport.IsValid());
	EXPECT_EQ(viewport.target_width, 1001);
	EXPECT_EQ(viewport.target_height, 250);
	EXPECT_EQ(viewport.timeline, (audio::DeviceRect { 0, 0, 1001, 23 }));
	EXPECT_EQ(viewport.scrollbar, (audio::DeviceRect { 0, 231, 1001, 19 }));
	EXPECT_EQ(viewport.content, (audio::DeviceRect { 0, 23, 1001, 208 }));
	EXPECT_DOUBLE_EQ(viewport.first_column_exact, 3.75);
	EXPECT_EQ(viewport.first_column, 3u);
	EXPECT_DOUBLE_EQ(viewport.first_column_offset, -0.75);
	EXPECT_EQ(viewport.visible_column_count, 1002u);
	EXPECT_DOUBLE_EQ(viewport.milliseconds_per_column, 16.0);
}

TEST(skia_audio_frame_model, viewport_clamps_scroll_to_audio_extent) {
	audio::FrameViewportRequest request;
	request.logical_width = 500;
	request.logical_height = 160;
	request.content_scale = 1.5;
	request.timeline_height = 16;
	request.scroll_left = 100000;
	request.duration_ms = 10000;
	request.milliseconds_per_logical_pixel = 10.0;

	auto const viewport = audio::BuildFrameViewport(request);
	ASSERT_TRUE(viewport.IsValid());
	EXPECT_EQ(viewport.logical_audio_width, 1000);
	EXPECT_EQ(viewport.scroll_left, 500);
	EXPECT_EQ(viewport.first_column, 750u);
}

TEST(skia_audio_frame_model, invalid_or_collapsed_viewports_are_rejected) {
	audio::FrameViewportRequest request;
	request.logical_width = 100;
	request.logical_height = 20;
	request.content_scale = 1.0;
	request.timeline_height = 10;
	request.scrollbar_height = 10;
	request.duration_ms = 1000;
	request.milliseconds_per_logical_pixel = 20.0;
	EXPECT_FALSE(audio::BuildFrameViewport(request).IsValid());

	request.logical_height = 100;
	request.content_scale = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(audio::BuildFrameViewport(request).IsValid());
}

TEST(skia_audio_frame_model, style_spans_clip_merge_and_apply_priority_in_device_pixels) {
	audio::FrameViewportRequest request;
	request.logical_width = 100;
	request.logical_height = 120;
	request.content_scale = 1.25;
	request.timeline_height = 10;
	request.scrollbar_height = 10;
	request.scroll_left = 3;
	request.duration_ms = 3000;
	request.milliseconds_per_logical_pixel = 20.0;
	auto const viewport = audio::BuildFrameViewport(request);
	ASSERT_TRUE(viewport.IsValid());

	std::vector<audio::TimeStyleRange> ranges {
		{ 0, 200, audio::FrameStyle::Inactive },
		{ 100, 180, audio::FrameStyle::Selected },
		{ 120, 140, audio::FrameStyle::Primary },
	};
	auto const spans = audio::BuildDeviceStyleSpans(ranges, viewport);
	ASSERT_EQ(6u, spans.size());
	EXPECT_EQ(audio::FrameStyle::Inactive, spans[0].style);
	EXPECT_NEAR(0.f, spans[0].x, 0.001f);
	EXPECT_NEAR(2.5f, spans[0].width, 0.001f);
	EXPECT_EQ(audio::FrameStyle::Selected, spans[1].style);
	EXPECT_EQ(audio::FrameStyle::Primary, spans[2].style);
	EXPECT_EQ(audio::FrameStyle::Selected, spans[3].style);
	EXPECT_EQ(audio::FrameStyle::Inactive, spans[4].style);
	EXPECT_EQ(audio::FrameStyle::Normal, spans[5].style);
	EXPECT_NEAR(static_cast<float>(viewport.content.width),
		spans.back().x + spans.back().width, 0.001f);
}

TEST(skia_audio_frame_model, empty_style_ranges_cover_the_visible_content_as_normal) {
	audio::FrameViewportRequest request;
	request.logical_width = 320;
	request.logical_height = 120;
	request.content_scale = 1.5;
	request.timeline_height = 10;
	request.scrollbar_height = 10;
	request.duration_ms = 3000;
	request.milliseconds_per_logical_pixel = 20.0;
	auto const viewport = audio::BuildFrameViewport(request);
	ASSERT_TRUE(viewport.IsValid());
	auto const spans = audio::BuildDeviceStyleSpans({}, viewport);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(audio::FrameStyle::Normal, spans[0].style);
	EXPECT_FLOAT_EQ(static_cast<float>(viewport.content.x), spans[0].x);
	EXPECT_FLOAT_EQ(static_cast<float>(viewport.content.width), spans[0].width);
}

TEST(skia_audio_frame_model, legacy_linear_band_plan_matches_range_and_interpolation_rules) {
	audio::SpectrumBandPlanRequest request;
	request.bin_count = 16;
	request.output_height = 4;
	request.sample_rate = 48000;

	auto ranged = audio::BuildSpectrumBandPlan(request);
	ASSERT_TRUE(ranged.IsValid());
	EXPECT_FALSE(ranged.interpolated);
	EXPECT_EQ(ranged.bands[0], (audio::SpectrumBand { 0, 4, 0.f }));
	EXPECT_EQ(ranged.bands[3], (audio::SpectrumBand { 12, 15, 0.f }));

	request.output_height = 32;
	auto interpolated = audio::BuildSpectrumBandPlan(request);
	ASSERT_TRUE(interpolated.IsValid());
	EXPECT_TRUE(interpolated.interpolated);
	EXPECT_EQ(interpolated.bands.front(), (audio::SpectrumBand { 0, 1, 0.5f }));
	EXPECT_EQ(interpolated.bands.back(), (audio::SpectrumBand { 15, 15, 0.f }));
}

TEST(skia_audio_frame_model, frequency_curve_plan_is_monotonic_bounded_and_revisioned) {
	audio::SpectrumBandPlanRequest request;
	request.bin_count = 512;
	request.output_height = 180;
	request.sample_rate = 48000;
	request.mode = audio::SpectrumScaleMode::FrequencyCurve;
	request.frequency_reference_position = audio::SpectrumFrequencyReferenceForPreset(2);

	auto const plan = audio::BuildSpectrumBandPlan(request);
	ASSERT_TRUE(plan.IsValid());
	EXPECT_FALSE(plan.interpolated);
	for (std::size_t i = 1; i < plan.bands.size(); ++i) {
		EXPECT_LE(plan.bands[i - 1].first, plan.bands[i].first);
		EXPECT_LE(plan.bands[i - 1].last, plan.bands[i].last);
	}

	request.frequency_reference_position = audio::SpectrumFrequencyReferenceForPreset(4);
	auto const changed = audio::BuildSpectrumBandPlan(request);
	ASSERT_TRUE(changed.IsValid());
	EXPECT_NE(changed.revision, plan.revision);
	EXPECT_NE(changed.bands, plan.bands);
}

TEST(skia_audio_frame_model, invalid_spectrum_plans_are_rejected) {
	audio::SpectrumBandPlanRequest request;
	request.bin_count = 513;
	request.output_height = 100;
	request.sample_rate = 48000;
	EXPECT_FALSE(audio::BuildSpectrumBandPlan(request).IsValid());

	request.bin_count = 512;
	request.output_height = 0;
	EXPECT_FALSE(audio::BuildSpectrumBandPlan(request).IsValid());
}
