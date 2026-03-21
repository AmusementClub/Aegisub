#include <main.h>

#include "../../src/mkv_wrap_common.h"

#include <zlib.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace {
std::string CompressZlib(std::string_view input) {
	if (input.empty())
		return {};
	if (input.size() > std::numeric_limits<uInt>::max())
		throw std::runtime_error("input too large for test compressor");

	z_stream stream{};
	if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK)
		throw std::runtime_error("deflateInit failed");

	stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
	stream.avail_in = static_cast<uInt>(input.size());

	std::string output;
	std::array<char, 256> chunk;

	for (;;) {
		stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
		stream.avail_out = static_cast<uInt>(chunk.size());
		auto const code = deflate(&stream, Z_FINISH);
		if (code != Z_OK && code != Z_STREAM_END) {
			deflateEnd(&stream);
			throw std::runtime_error("deflate failed");
		}

		output.append(chunk.data(), chunk.size() - stream.avail_out);
		if (code == Z_STREAM_END)
			break;
	}

	deflateEnd(&stream);
	return output;
}
}

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

TEST(mkv_wrap_common, decode_content_encoded_data_handles_no_encodings) {
	std::string error;
	auto const decoded = DecodeMkvContentEncodedData("plain", {}, MkvContentEncodingTarget::Block, &error);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ("plain", *decoded);
	EXPECT_TRUE(error.empty());
}

TEST(mkv_wrap_common, decode_content_encoded_data_inflates_zlib_packets) {
	std::string error;
	std::vector<MkvContentEncoding> const encodings{
		{0, kMkvContentEncodingScopeBlock, MkvContentEncodingAlgorithm::Zlib, {}}
	};

	auto const decoded = DecodeMkvContentEncodedData(CompressZlib("Dialogue payload"), encodings, MkvContentEncodingTarget::Block, &error);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ("Dialogue payload", *decoded);
	EXPECT_TRUE(error.empty());
}

TEST(mkv_wrap_common, decode_content_encoded_data_restores_header_stripping) {
	std::string error;
	std::vector<MkvContentEncoding> const encodings{
		{0, kMkvContentEncodingScopeBlock, MkvContentEncodingAlgorithm::HeaderStripping, "prefix"}
	};

	auto const decoded = DecodeMkvContentEncodedData("payload", encodings, MkvContentEncodingTarget::Block, &error);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ("prefixpayload", *decoded);
	EXPECT_TRUE(error.empty());
}

TEST(mkv_wrap_common, decode_content_encoded_data_applies_descending_order) {
	std::string error;
	std::vector<MkvContentEncoding> const encodings{
		{0, kMkvContentEncodingScopeBlock, MkvContentEncodingAlgorithm::HeaderStripping, "prefix"},
		{1, kMkvContentEncodingScopeBlock, MkvContentEncodingAlgorithm::Zlib, {}}
	};

	auto const decoded = DecodeMkvContentEncodedData(CompressZlib("payload"), encodings, MkvContentEncodingTarget::Block, &error);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ("prefixpayload", *decoded);
	EXPECT_TRUE(error.empty());
}

TEST(mkv_wrap_common, decode_content_encoded_data_respects_target_scope) {
	std::string error;
	std::vector<MkvContentEncoding> const encodings{
		{0, kMkvContentEncodingScopePrivate, MkvContentEncodingAlgorithm::HeaderStripping, "prefix"}
	};

	auto const block = DecodeMkvContentEncodedData("payload", encodings, MkvContentEncodingTarget::Block, &error);
	ASSERT_TRUE(block.has_value());
	EXPECT_EQ("payload", *block);

	auto const priv = DecodeMkvContentEncodedData("payload", encodings, MkvContentEncodingTarget::Private, &error);
	ASSERT_TRUE(priv.has_value());
	EXPECT_EQ("prefixpayload", *priv);
}

TEST(mkv_wrap_common, decode_content_encoded_data_rejects_unsupported_algorithms) {
	std::string error;
	std::vector<MkvContentEncoding> const encodings{
		{0, kMkvContentEncodingScopeBlock, MkvContentEncodingAlgorithm::Unsupported, {}}
	};

	auto const decoded = DecodeMkvContentEncodedData("payload", encodings, MkvContentEncodingTarget::Block, &error);

	EXPECT_FALSE(decoded.has_value());
	EXPECT_EQ("unsupported content encoding algorithm", error);
}
