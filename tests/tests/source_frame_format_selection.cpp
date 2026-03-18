#include <main.h>

#include "../../src/source_frame_format_selection.h"

#include <gtest/gtest.h>

TEST(source_frame_format_selection, compatibility_mode_prefers_bgra8_when_available) {
	auto mode = SelectPreferredSourceFrameOutputMode(
		{ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 },
		{ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 },
		true);

	EXPECT_EQ(SourceFrameOutputMode::Bgra8, mode);
}

TEST(source_frame_format_selection, compatibility_mode_requires_bgra8_even_when_provider_does_not_advertise_it) {
	auto mode = SelectPreferredSourceFrameOutputMode(
		{ SourceFrameOutputMode::Native },
		{ SourceFrameOutputMode::Native },
		true);

	EXPECT_EQ(SourceFrameOutputMode::Bgra8, mode);
}

TEST(source_frame_format_selection, overlay_mode_prefers_native_when_supported) {
	auto mode = SelectPreferredSourceFrameOutputMode(
		{ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 },
		{ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 },
		false);

	EXPECT_EQ(SourceFrameOutputMode::Native, mode);
}

TEST(source_frame_format_selection, appends_bgra8_as_implicit_fallback) {
	auto mode = SelectPreferredSourceFrameOutputMode(
		{ SourceFrameOutputMode::Native },
		{ SourceFrameOutputMode::Bgra8 },
		false);

	EXPECT_EQ(SourceFrameOutputMode::Bgra8, mode);
}
