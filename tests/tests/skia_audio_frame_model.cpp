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

TEST(skia_audio_frame_model, zoom_preserves_cursor_or_viewport_center_time) {
	EXPECT_EQ(225, audio::AudioScrollLeftAfterZoom(100, 200, 20.0, 10.0, 2500.0));
	EXPECT_EQ(300, audio::AudioScrollLeftAfterZoom(100, 200, 20.0, 10.0));
	EXPECT_EQ(100, audio::AudioScrollLeftAfterZoom(100, 200, 20.0, 20.0, 2501.0));
	EXPECT_EQ(100, audio::AudioScrollLeftAfterZoom(
		100, 0, 20.0, 10.0, 2500.0));
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

TEST(skia_audio_frame_model, mouse_position_uses_only_the_logical_audio_content_area) {
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

	EXPECT_EQ(8060, audio::MousePositionMsForClientPoint(viewport, 400, 100, 1.25, 20.0));
	EXPECT_EQ(-1, audio::MousePositionMsForClientPoint(viewport, 400, 10, 1.25, 20.0));
	EXPECT_EQ(-1, audio::MousePositionMsForClientPoint(viewport, 400, 190, 1.25, 20.0));
	EXPECT_EQ(-1, audio::MousePositionMsForClientPoint(viewport, -1, 100, 1.25, 20.0));
	EXPECT_EQ(-1, audio::MousePositionMsForClientPoint(viewport, 801, 100, 1.25, 20.0));
	EXPECT_EQ(-1, audio::MousePositionMsForClientPoint(viewport, 400, 100, 0.0, 20.0));
}

TEST(skia_audio_frame_model, cursor_placement_prefers_playback_and_uses_device_viewport_coordinates) {
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

	auto const playback = audio::BuildCursorPlacement(viewport, 400, 800);
	EXPECT_EQ(audio::CursorSource::Playback, playback.source);
	EXPECT_EQ(800, playback.position_ms);
	EXPECT_NEAR(46.25f, playback.device_x, 0.001f);
	EXPECT_STREQ("playback", audio::CursorSourceName(playback.source));

	auto const mouse = audio::BuildCursorPlacement(viewport, 400, -1);
	EXPECT_EQ(audio::CursorSource::Mouse, mouse.source);
	EXPECT_EQ(400, mouse.position_ms);
	EXPECT_NEAR(21.25f, mouse.device_x, 0.001f);
	EXPECT_STREQ("mouse", audio::CursorSourceName(mouse.source));
}

TEST(skia_audio_frame_model, cursor_placement_hides_when_no_live_source_or_viewport) {
	audio::FrameViewportRequest request;
	request.logical_width = 320;
	request.logical_height = 120;
	request.content_scale = 1.0;
	request.timeline_height = 10;
	request.scrollbar_height = 10;
	request.duration_ms = 3000;
	request.milliseconds_per_logical_pixel = 20.0;
	auto const viewport = audio::BuildFrameViewport(request);
	ASSERT_TRUE(viewport.IsValid());

	auto const hidden = audio::BuildCursorPlacement(viewport, -1, -1);
	EXPECT_FALSE(hidden.IsActive());
	EXPECT_EQ(-1, hidden.position_ms);
	EXPECT_STREQ("none", audio::CursorSourceName(hidden.source));
	EXPECT_FALSE(audio::BuildCursorPlacement({}, 100, -1).IsActive());
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

TEST(skia_audio_frame_model, scrollbar_geometry_matches_legacy_layer_positions_and_minimum_thumb) {
	auto const geometry = audio::BuildScrollbarGeometry(
		100.f,
		10.f,
		25.f,
		1000,
		20,
		200,
		500,
		300,
		200);

	ASSERT_TRUE(geometry.valid);
	EXPECT_FLOAT_EQ(16.f, geometry.thumb_x);
	EXPECT_FLOAT_EQ(2.f, geometry.nominal_thumb_width);
	EXPECT_FLOAT_EQ(10.f, geometry.thumb_width);
	EXPECT_TRUE(geometry.selection_visible);
	EXPECT_FLOAT_EQ(30.f, geometry.selection_x);
	EXPECT_FLOAT_EQ(20.f, geometry.selection_width);
	EXPECT_TRUE(geometry.load_visible);
	EXPECT_FLOAT_EQ(25.f, geometry.load_x);
	EXPECT_FLOAT_EQ(25.f, geometry.load_width);
}

TEST(skia_audio_frame_model, presentation_interval_tracks_high_refresh_displays_without_a_60hz_cap) {
	EXPECT_EQ(std::chrono::nanoseconds(16'666'666), audio::PresentationFrameInterval(60));
	EXPECT_EQ(std::chrono::nanoseconds(6'944'444), audio::PresentationFrameInterval(144));
	EXPECT_EQ(std::chrono::nanoseconds(4'166'666), audio::PresentationFrameInterval(240));
	EXPECT_EQ(audio::PresentationFrameInterval(60), audio::PresentationFrameInterval(0));
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

TEST(skia_audio_frame_model, style_span_sweep_preserves_overlaps_and_shared_boundaries) {
	audio::FrameViewportRequest request;
	request.logical_width = 200;
	request.logical_height = 120;
	request.content_scale = 1.0;
	request.timeline_height = 10;
	request.scrollbar_height = 10;
	request.duration_ms = 4000;
	request.milliseconds_per_logical_pixel = 10.0;
	auto const viewport = audio::BuildFrameViewport(request);
	ASSERT_TRUE(viewport.IsValid());

	std::vector<audio::TimeStyleRange> ranges {
		{ 0, 500, audio::FrameStyle::Inactive },
		{ 100, 300, audio::FrameStyle::Selected },
		{ 200, 400, audio::FrameStyle::Selected },
		{ 300, 350, audio::FrameStyle::Primary },
		{ 500, 700, audio::FrameStyle::Primary },
	};
	auto const spans = audio::BuildDeviceStyleSpans(ranges, viewport);
	ASSERT_EQ(7u, spans.size());
	EXPECT_EQ(audio::FrameStyle::Inactive, spans[0].style);
	EXPECT_EQ(audio::FrameStyle::Selected, spans[1].style);
	EXPECT_EQ(audio::FrameStyle::Primary, spans[2].style);
	EXPECT_EQ(audio::FrameStyle::Selected, spans[3].style);
	EXPECT_EQ(audio::FrameStyle::Inactive, spans[4].style);
	EXPECT_EQ(audio::FrameStyle::Primary, spans[5].style);
	EXPECT_EQ(audio::FrameStyle::Normal, spans[6].style);
	EXPECT_FLOAT_EQ(0.f, spans[0].x);
	EXPECT_FLOAT_EQ(10.f, spans[0].width);
	EXPECT_FLOAT_EQ(20.f, spans[1].width);
	EXPECT_FLOAT_EQ(5.f, spans[2].width);
	EXPECT_FLOAT_EQ(5.f, spans[3].width);
	EXPECT_FLOAT_EQ(10.f, spans[4].width);
	EXPECT_FLOAT_EQ(20.f, spans[5].width);
	EXPECT_FLOAT_EQ(130.f, spans[6].width);
}

TEST(skia_audio_frame_model, style_span_validation_tolerates_float_boundary_reconstruction) {
	constexpr float content_width = 300.f;
	constexpr float span_x = 61.00827789306640625f;
	constexpr float span_width = 238.991729736328125f;

	// Rearranging the right-bound check as x <= right - width loses one ULP
	// here even though the reconstructed endpoint is the content boundary.
	EXPECT_GT(span_x, content_width - span_width);
	EXPECT_TRUE(audio::IsValidDeviceStyleSpan(0.f, content_width, span_x, span_width));
	EXPECT_FALSE(audio::IsValidDeviceStyleSpan(0.f, content_width, span_x, span_width + 1.f));
	EXPECT_FALSE(audio::IsValidDeviceStyleSpan(0.f, content_width, span_x, 0.f));
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
