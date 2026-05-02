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

TEST(lagi_ass_style, parses_compatible_integer_fields_and_normalizes_output) {
	AssStyle style("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1x,0,0,0,100,100,0,0,1,2,2,2,&H10px,-20tail,+30more,0x81junk");

	EXPECT_TRUE(style.bold);
	EXPECT_FALSE(style.italic);
	EXPECT_EQ(16, style.Margin[0]);
	EXPECT_EQ(-20, style.Margin[1]);
	EXPECT_EQ(30, style.Margin[2]);
	EXPECT_EQ(129, style.encoding);
	EXPECT_EQ(
		"Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,16,-20,30,129",
		style.GetEntryData());
}

TEST(lagi_ass_style, preserves_negative_style_values_in_memory_and_clamps_saved_output) {
	AssStyle style("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,-10,-20,-3,0,1,-4,-5,2,10,20,30,1");

	EXPECT_DOUBLE_EQ(-10.0, style.scalex);
	EXPECT_DOUBLE_EQ(-20.0, style.scaley);
	EXPECT_DOUBLE_EQ(-3.0, style.spacing);
	EXPECT_DOUBLE_EQ(-4.0, style.outline_w);
	EXPECT_DOUBLE_EQ(-5.0, style.shadow_w);
	EXPECT_EQ(
		"Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,0,0,0,0,1,0,0,2,10,20,30,1",
		style.GetEntryData());
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
