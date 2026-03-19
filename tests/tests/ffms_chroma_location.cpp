#include <main.h>

#include "../../src/ffms_chroma_location.h"

#include <gtest/gtest.h>

TEST(ffms_chroma_location, mirrors_ffmpeg_numeric_mapping) {
	EXPECT_EQ(SourceFrameChromaLocation::Unknown, ffms::MapChromaLocation(0));
	EXPECT_EQ(SourceFrameChromaLocation::Left, ffms::MapChromaLocation(1));
	EXPECT_EQ(SourceFrameChromaLocation::Center, ffms::MapChromaLocation(2));
	EXPECT_EQ(SourceFrameChromaLocation::TopLeft, ffms::MapChromaLocation(3));
	EXPECT_EQ(SourceFrameChromaLocation::TopCenter, ffms::MapChromaLocation(4));
	EXPECT_EQ(SourceFrameChromaLocation::BottomLeft, ffms::MapChromaLocation(5));
	EXPECT_EQ(SourceFrameChromaLocation::BottomCenter, ffms::MapChromaLocation(6));
	EXPECT_EQ(SourceFrameChromaLocation::Unknown, ffms::MapChromaLocation(7));
	EXPECT_EQ(SourceFrameChromaLocation::Unknown, ffms::MapChromaLocation(-1));
	EXPECT_EQ(SourceFrameChromaLocation::Unknown, ffms::MapChromaLocation(999));
}
