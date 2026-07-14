// Copyright (c) 2026, Aegisub Project

#include <main.h>

#include "../../src/pgs_sup_packet_stream.h"

#include <cstdint>
#include <limits>
#include <string>

namespace {
void AppendBe32(std::string& value, uint32_t number) {
	value.push_back(static_cast<char>(number >> 24));
	value.push_back(static_cast<char>(number >> 16));
	value.push_back(static_cast<char>(number >> 8));
	value.push_back(static_cast<char>(number));
}

void AppendSegment(std::string& value, uint32_t pts, uint32_t dts, unsigned char type, std::string payload) {
	value += "PG";
	AppendBe32(value, pts);
	AppendBe32(value, dts);
	value.push_back(static_cast<char>(type));
	value.push_back(static_cast<char>(payload.size() >> 8));
	value.push_back(static_cast<char>(payload.size()));
	value += payload;
}
}

TEST(pgs_sup_packet_stream, parses_segments_and_90khz_timestamps) {
	std::string data;
	AppendSegment(data, 90000, 0, 0x16, "pcs");
	AppendSegment(data, 180000, 90000, 0x80, "");

	auto stream = ParsePgsSupPacketStream(data);
	ASSERT_EQ(kSecondarySubtitleCodecHdmvPgs, stream.codec_id);
	ASSERT_EQ(2u, stream.packets.size());
	EXPECT_EQ(1000000000, stream.packets[0].pts_ns);
	EXPECT_EQ(kSecondarySubtitleTimestampUnknown, stream.packets[0].dts_ns);
	EXPECT_EQ(2000000000, stream.packets[1].pts_ns);
	EXPECT_EQ(1000000000, stream.packets[1].dts_ns);
	EXPECT_EQ(std::string("\x16\0\3pcs", 6), stream.packets[0].payload);
}

TEST(pgs_sup_packet_stream, unwraps_32bit_timestamp_rollover) {
	std::string data;
	AppendSegment(data, 0xfffffff0u, 0, 0x80, "");
	AppendSegment(data, 0x00000010u, 0, 0x80, "");

	auto stream = ParsePgsSupPacketStream(data);
	EXPECT_LT(stream.packets[0].pts_ns, stream.packets[1].pts_ns);
	EXPECT_EQ(32 * 1000000000LL / 90000, stream.packets[1].pts_ns - stream.packets[0].pts_ns);
}

TEST(pgs_sup_packet_stream, saturates_when_fractional_second_exceeds_int64_range) {
	auto const max_ns = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	auto const max_seconds = max_ns / 1000000000ULL;
	auto const whole_second_ticks = max_seconds * 90000ULL;

	EXPECT_EQ(
		static_cast<int64_t>(max_seconds * 1000000000ULL),
		pgs_sup::TicksToNanoseconds(whole_second_ticks));
	EXPECT_EQ(
		std::numeric_limits<int64_t>::max(),
		pgs_sup::TicksToNanoseconds(whole_second_ticks + 89999ULL));
}

TEST(pgs_sup_packet_stream, rejects_truncated_and_invalid_input) {
	EXPECT_THROW(ParsePgsSupPacketStream(""), PgsSupParseError);
	EXPECT_THROW(ParsePgsSupPacketStream("not a sup file"), PgsSupParseError);

	std::string data;
	AppendSegment(data, 0, 0, 0x16, "abc");
	data.pop_back();
	EXPECT_THROW(ParsePgsSupPacketStream(data), PgsSupParseError);
}
