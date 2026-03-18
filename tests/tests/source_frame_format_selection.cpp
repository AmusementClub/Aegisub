#include <main.h>

#include "../../src/source_frame_format_selection.h"

#include <gtest/gtest.h>

TEST(source_frame_format_selection, compatibility_mode_prefers_bgra8_when_available) {
	auto format = SelectPreferredSourceFrameFormat(
		{ SourceFramePixelFormat::YCbCr420P10, SourceFramePixelFormat::Bgra8 },
		{ SourceFramePixelFormat::YCbCr420P10, SourceFramePixelFormat::Bgra8 },
		true);

	EXPECT_EQ(SourceFramePixelFormat::Bgra8, format);
}

TEST(source_frame_format_selection, compatibility_mode_requires_bgra8_even_when_provider_does_not_advertise_it) {
	auto format = SelectPreferredSourceFrameFormat(
		{ SourceFramePixelFormat::YCbCr420P10 },
		{ SourceFramePixelFormat::YCbCr420P10 },
		true);

	EXPECT_EQ(SourceFramePixelFormat::Bgra8, format);
}

TEST(source_frame_format_selection, overlay_mode_prefers_first_supported_requested_format) {
	auto format = SelectPreferredSourceFrameFormat(
		{ SourceFramePixelFormat::P010, SourceFramePixelFormat::YCbCr420P10, SourceFramePixelFormat::Bgra8 },
		{ SourceFramePixelFormat::YCbCr420P10, SourceFramePixelFormat::Bgra8 },
		false);

	EXPECT_EQ(SourceFramePixelFormat::YCbCr420P10, format);
}

TEST(source_frame_format_selection, appends_bgra8_as_implicit_fallback) {
	auto format = SelectPreferredSourceFrameFormat(
		{ SourceFramePixelFormat::P010 },
		{ SourceFramePixelFormat::Bgra8 },
		false);

	EXPECT_EQ(SourceFramePixelFormat::Bgra8, format);
}
