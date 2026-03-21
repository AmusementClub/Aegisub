#include <main.h>

#include "../../src/mkv_wrap_common.h"

TEST(mkv_wrap_common, classify_supported_codecs) {
	EXPECT_EQ(MkvTextSubtitleCodec::Ass, ClassifyMkvTextSubtitleCodec("S_TEXT/ASS"));
	EXPECT_EQ(MkvTextSubtitleCodec::Ssa, ClassifyMkvTextSubtitleCodec("S_TEXT/SSA"));
	EXPECT_EQ(MkvTextSubtitleCodec::Utf8, ClassifyMkvTextSubtitleCodec("S_TEXT/UTF8"));
	EXPECT_EQ(MkvTextSubtitleCodec::Unsupported, ClassifyMkvTextSubtitleCodec("S_TEXT/WEBVTT"));
}

TEST(mkv_wrap_common, split_codec_private_lines_on_crlf_and_lf) {
	auto const lines = SplitMkvCodecPrivateLines("[Script Info]\r\nTitle: Test\n\n[V4+ Styles]\r\n");

	ASSERT_EQ(3u, lines.size());
	EXPECT_EQ("[Script Info]", lines[0]);
	EXPECT_EQ("Title: Test", lines[1]);
	EXPECT_EQ("[V4+ Styles]", lines[2]);
}

TEST(mkv_wrap_common, parse_ass_packet_uses_read_order_and_layer) {
	auto const line = ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec::Ass, "17,3,Default,,0,0,0,,Hello", 1000, 2500, 0);

	ASSERT_TRUE(line.has_value());
	EXPECT_EQ(17, line->sort_key);
	EXPECT_EQ("Dialogue: 3,0:00:01.00,0:00:02.50,Default,,0,0,0,,Hello", line->line);
}

TEST(mkv_wrap_common, parse_ass_packet_rejects_bad_numeric_fields) {
	EXPECT_FALSE(ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec::Ass, "bad,3,Default,,0,0,0,,Hello", 0, 1000, 0).has_value());
	EXPECT_FALSE(ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec::Ass, "1,bad,Default,,0,0,0,,Hello", 0, 1000, 0).has_value());
	EXPECT_FALSE(ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec::Ass, "1", 0, 1000, 0).has_value());
}

TEST(mkv_wrap_common, parse_utf8_packet_escapes_newlines) {
	auto const line = ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec::Utf8, "Line 1\r\nLine 2\nLine 3", 50, 3050, 9);

	ASSERT_TRUE(line.has_value());
	EXPECT_EQ(9, line->sort_key);
	EXPECT_EQ("Dialogue: 0,0:00:00.05,0:00:03.05,Default,,0,0,0,,Line 1\\NLine 2\\NLine 3", line->line);
}
