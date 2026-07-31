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

TEST(lagi_ass_style, clamps_margins_to_vsfilter_style_editor_range) {
	AssStyle low(
		"Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,"
		"-1,0,0,0,100,100,0,0,1,2,2,2,-10001,-10000,10001,1");
	EXPECT_EQ(AssStyle::MinMargin, low.Margin[0]);
	EXPECT_EQ(AssStyle::MinMargin, low.Margin[1]);
	EXPECT_EQ(AssStyle::MaxMargin, low.Margin[2]);

	AssStyle high(
		"Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,"
		"-1,0,0,0,100,100,0,0,1,2,2,2,10000,99999,-99999,1");
	EXPECT_EQ(AssStyle::MaxMargin, high.Margin[0]);
	EXPECT_EQ(AssStyle::MaxMargin, high.Margin[1]);
	EXPECT_EQ(AssStyle::MinMargin, high.Margin[2]);
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

TEST(lagi_ass_style, parses_compatible_float_fields_and_normalizes_output) {
	AssStyle style("Style: Default,Arial,48.5px,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,+110.25tail,99.5%,1.25em,-15deg,1,2.5px,3.75drop,2,10,20,30,1");

	EXPECT_DOUBLE_EQ(48.5, style.fontsize);
	EXPECT_DOUBLE_EQ(110.25, style.scalex);
	EXPECT_DOUBLE_EQ(99.5, style.scaley);
	EXPECT_DOUBLE_EQ(1.25, style.spacing);
	EXPECT_DOUBLE_EQ(-15.0, style.angle);
	EXPECT_DOUBLE_EQ(2.5, style.outline_w);
	EXPECT_DOUBLE_EQ(3.75, style.shadow_w);
	EXPECT_EQ(
		"Style: Default,Arial,48.5,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,110.25,99.5,1.25,-15,1,2.5,3.75,2,10,20,30,1",
		style.GetEntryData());
}

TEST(lagi_ass_style, parses_compatible_color_fields_and_normalizes_output) {
	AssStyle style("Style: Default,Arial,48,0x11223344tail,16777215more,&H00010203junk,-1,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1");

	EXPECT_EQ(agi::Color(0x44, 0x33, 0x22, 0x11), style.primary);
	EXPECT_EQ(agi::Color(0xFF, 0xFF, 0xFF, 0x00), style.secondary);
	EXPECT_EQ(agi::Color(0x03, 0x02, 0x01, 0x00), style.outline);
	EXPECT_EQ(agi::Color(0xFF, 0xFF, 0xFF, 0xFF), style.shadow);
	EXPECT_EQ(
		"Style: Default,Arial,48,&H11223344,&H00FFFFFF,&H00010203,&HFFFFFFFF,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1",
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

TEST(lagi_ass_style, rejects_bad_color_field) {
	EXPECT_THROW(
		AssStyle("Style: Default,Arial,48,nope,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"),
		SubtitleFormatParseError);
}
