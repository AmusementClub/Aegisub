#include <gtest/gtest.h>

#include "../../src/ass_style.h"
#include "../../src/subtitle_format.h"

TEST(lagi_ass_style, parses_valid_style) {
	AssStyle style("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1");

	EXPECT_EQ("Default", style.name);
	EXPECT_EQ("Arial", style.font);
	EXPECT_DOUBLE_EQ(48.0, style.fontsize);
	EXPECT_TRUE(style.bold);
	EXPECT_FALSE(style.italic);
	EXPECT_EQ(10, style.Margin[0]);
	EXPECT_EQ(20, style.Margin[1]);
	EXPECT_EQ(30, style.Margin[2]);
	EXPECT_EQ(1, style.encoding);
}

TEST(lagi_ass_style, rejects_bad_int_field) {
	EXPECT_THROW(
		AssStyle("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,nope,20,30,1"),
		SubtitleFormatParseError);
}

TEST(lagi_ass_style, rejects_bad_double_field) {
	EXPECT_THROW(
		AssStyle("Style: Default,Arial,nope,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"),
		SubtitleFormatParseError);
}
