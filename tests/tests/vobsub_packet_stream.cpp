#include <main.h>

#include "../../src/vobsub_packet_stream.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr size_t kSectorSize = 2048;

constexpr char kPalette[] =
	"000000, ff0000, 00ff00, 0000ff, ffffff, 808080, 800000, 008000, "
	"000080, 808000, 800080, 008080, c0c0c0, ff8080, 80ff80, 8080ff";

void AppendBe16(std::string& value, uint16_t number) {
	value.push_back(static_cast<char>(number >> 8));
	value.push_back(static_cast<char>(number));
}

std::string MakePes(unsigned char substream_id, std::string_view fragment) {
	std::string pes("\0\0\1\xbd", 4);
	auto const packet_length = static_cast<uint16_t>(3 + 1 + fragment.size());
	AppendBe16(pes, packet_length);
	pes.push_back(static_cast<char>(0x80)); // MPEG-2 PES marker bits
	pes.push_back(0);                       // no PTS/DTS; IDX time is authoritative
	pes.push_back(0);                       // no optional-header data
	pes.push_back(static_cast<char>(substream_id));
	pes.append(fragment);
	return pes;
}

std::string MakeSector(std::initializer_list<std::string_view> packets) {
	// The parser needs only the MPEG-2 marker form and stuffing length from the
	// pack header, but use a correctly-sized 14-byte header as real VobSub does.
	std::string sector("\0\0\1\xba", 4);
	sector.append("\x44\0\0\0\0\0\0\0\0\0", 10);
	for (auto packet : packets)
		sector.append(packet);

	if (sector.size() > kSectorSize - 6)
		throw std::logic_error("Test VobSub sector is too large.");
	std::string padding("\0\0\1\xbe", 4);
	auto const padding_size = static_cast<uint16_t>(kSectorSize - sector.size() - 6);
	AppendBe16(padding, padding_size);
	padding.append(padding_size, static_cast<char>(0xff));
	sector += padding;
	return sector;
}

std::string MakeSpu() {
	return std::string(
		"\x00\x24\x00\x06\x90\x90\x00\x00\x00\x1e"
		"\x03\x00\x10\x04\x00\xf0"
		"\x05\x00\x00\x01\x00\x00\x01"
		"\x06\x00\x04\x00\x05\x01\xff"
		"\x00\x5a\x00\x1e\x02\xff",
		36);
}

std::string MakeSingleTrackIndex(std::string_view filepos = "000000000") {
	std::string idx =
		"# VobSub index file, v7 (do not modify this line!)\n"
		"size: 720x480\n"
		"palette: ";
	idx += kPalette;
	idx +=
		"\n"
		"id: en, index: 0\n"
		"timestamp: 00:00:01:000, filepos: ";
	idx += filepos;
	idx.push_back('\n');
	return idx;
}
}

TEST(vobsub_packet_stream, parses_tracks_default_codec_private_and_cumulative_delay) {
	std::string idx =
		"# VobSub index file, v7 (do not modify this line!)\r\n"
		"size: 720x480\r\n"
		"palette: ";
	idx += kPalette;
	idx +=
		"\r\n"
		"langidx: 1\r\n"
		"id: en, index: 0\r\n"
		"timestamp: 00:00:01:000, filepos: 000000000\r\n"
		"id: ja, index: 1\r\n"
		"alt: Japanese signs\r\n"
		"delay: +00:00:00:250\r\n"
		"delay: -00:00:00:050\r\n"
		"timestamp: 00:00:02:000, filepos: 000000800\r\n";

	auto parsed = ParseVobSubIndex(idx);
	ASSERT_EQ(2u, parsed.tracks.size());
	EXPECT_EQ(720, parsed.fallback_canvas_width);
	EXPECT_EQ(480, parsed.fallback_canvas_height);
	EXPECT_EQ(1, parsed.default_track_index);
	EXPECT_FALSE(parsed.tracks[0].is_default);
	EXPECT_TRUE(parsed.tracks[1].is_default);
	EXPECT_EQ("en", parsed.tracks[0].language);
	EXPECT_EQ("ja", parsed.tracks[1].language);
	EXPECT_EQ("Japanese signs", parsed.tracks[1].name);
	ASSERT_EQ(1u, parsed.tracks[0].packets.size());
	ASSERT_EQ(1u, parsed.tracks[1].packets.size());
	EXPECT_EQ(1000000000, parsed.tracks[0].packets[0].pts_ns);
	EXPECT_EQ(2200000000, parsed.tracks[1].packets[0].pts_ns);
	EXPECT_EQ(0x800u, parsed.tracks[1].packets[0].filepos);
	EXPECT_NE(std::string::npos, parsed.codec_private.find("size: 720x480"));
	EXPECT_NE(std::string::npos, parsed.codec_private.find("palette: 000000"));
	EXPECT_EQ(std::string::npos, parsed.codec_private.find("id: en"));
}

TEST(vobsub_packet_stream, assembles_complete_spu_across_pes_sectors_and_filters_substreams) {
	auto const spu = MakeSpu();
	auto const first_fragment = std::string_view(spu).substr(0, 11);
	auto const second_fragment = std::string_view(spu).substr(11);
	auto const first_pes = MakePes(0x20, first_fragment);
	auto const unrelated_pes = MakePes(0x21, "not part of the selected SPU");
	auto const second_pes = MakePes(0x20, second_fragment);
	std::string sub = MakeSector({first_pes, unrelated_pes});
	sub += MakeSector({second_pes});

	auto stream = ParseVobSubPacketStream(MakeSingleTrackIndex(), sub, 0);
	EXPECT_EQ(kSecondarySubtitleCodecDvdSubtitle, stream.codec_id);
	EXPECT_EQ(720, stream.fallback_canvas_width);
	EXPECT_EQ(480, stream.fallback_canvas_height);
	ASSERT_EQ(1u, stream.packets.size());
	EXPECT_EQ(1000000000, stream.packets[0].pts_ns);
	EXPECT_EQ(kSecondarySubtitleTimestampUnknown, stream.packets[0].dts_ns);
	EXPECT_EQ(spu, stream.packets[0].payload);
}

TEST(vobsub_packet_stream, selects_langidx_by_default) {
	auto const spu = MakeSpu();
	auto const english = MakePes(0x20, spu);
	auto japanese_spu = spu;
	japanese_spu[6] = static_cast<char>(0x5a);
	auto const japanese = MakePes(0x21, japanese_spu);
	std::string sub = MakeSector({english});
	sub += MakeSector({japanese});

	std::string idx =
		"# VobSub index file, v7 (do not modify this line!)\n"
		"langidx: 1\n"
		"id: en, index: 0\n"
		"timestamp: 00:00:01:000, filepos: 000000000\n"
		"id: ja, index: 1\n"
		"timestamp: 00:00:02:000, filepos: 000000800\n";

	auto stream = ParseVobSubPacketStream(idx, sub);
	ASSERT_EQ(1u, stream.packets.size());
	EXPECT_EQ(2000000000, stream.packets[0].pts_ns);
	EXPECT_EQ(japanese_spu, stream.packets[0].payload);
}

TEST(vobsub_packet_stream, accepts_global_delay_before_first_language_id) {
	std::string idx =
		"# VobSub index file, v7\n"
		"size: 720x480\n"
		"delay: +00:00:01:250\n"
		"id: en, index: 0\n"
		"timestamp: 00:00:02:000, filepos: 000000000\n";

	auto parsed = ParseVobSubIndex(idx);
	ASSERT_EQ(1u, parsed.tracks.size());
	ASSERT_EQ(1u, parsed.tracks[0].packets.size());
	EXPECT_EQ(3250000000, parsed.tracks[0].packets[0].pts_ns);
}

TEST(vobsub_packet_stream, normalizes_codec_private_keys_for_ffmpeg) {
	std::string idx =
		"# VobSub index file, v7\n"
		"SIZE: 720x480\n"
		"PALETTE: ";
	idx += kPalette;
	idx +=
		"\n"
		"id: en, index: 0\n"
		"timestamp: 00:00:00:000, filepos: 000000000\n";

	auto parsed = ParseVobSubIndex(idx);
	EXPECT_NE(std::string::npos, parsed.codec_private.find("size: 720x480"));
	EXPECT_NE(std::string::npos, parsed.codec_private.find("palette: 000000"));
	EXPECT_EQ(std::string::npos, parsed.codec_private.find("SIZE:"));
	EXPECT_EQ(std::string::npos, parsed.codec_private.find("PALETTE:"));
}

TEST(vobsub_packet_stream, accepts_hddvd_four_byte_spu_length) {
	std::string hd_spu("\0\0\0\0\0\x0a", 6);
	hd_spu += "data";
	auto const pes = MakePes(0x20, hd_spu);
	auto const sub = MakeSector({pes});

	auto stream = ParseVobSubPacketStream(MakeSingleTrackIndex(), sub, 0);
	ASSERT_EQ(1u, stream.packets.size());
	EXPECT_EQ(hd_spu, stream.packets[0].payload);
}

TEST(vobsub_packet_stream, rejects_bad_index_fields_and_truncated_sub_data) {
	EXPECT_THROW(ParseVobSubIndex("not an idx file"), VobSubParseError);
	EXPECT_THROW(ParseVobSubIndex(
		"# VobSub index file, v7\n"
		"id: en, index: 0\n"
		"timestamp: 00:60:00:000, filepos: 0\n"), VobSubParseError);
	EXPECT_THROW(ParseVobSubIndex(
		"# VobSub index file, v7\n"
		"id: en, index: 0\n"
		"timestamp: 00:00:00:000, filepos: xyz\n"), VobSubParseError);
	EXPECT_THROW(ParseVobSubPacketStream(MakeSingleTrackIndex(), "", 0), VobSubParseError);

	auto const spu = MakeSpu();
	auto const pes = MakePes(0x20, std::string_view(spu).substr(0, 8));
	auto const truncated = MakeSector({pes});
	EXPECT_THROW(ParseVobSubPacketStream(MakeSingleTrackIndex(), truncated, 0), VobSubParseError);
}
