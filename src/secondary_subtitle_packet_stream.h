#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

inline constexpr char const *kSecondarySubtitleCodecHdmvPgs = "hdmv-pgs";
inline constexpr char const *kSecondarySubtitleCodecDvdSubtitle = "dvd-subtitle";
inline constexpr int64_t kSecondarySubtitleTimestampUnknown = std::numeric_limits<int64_t>::min();

enum SecondarySubtitlePacketFlag : uint32_t {
	SecondarySubtitlePacketFlagNone = 0,
	SecondarySubtitlePacketFlagDiscontinuity = 1u << 0,
};

struct SecondarySubtitlePacket {
	int64_t pts_ns = kSecondarySubtitleTimestampUnknown;
	int64_t dts_ns = kSecondarySubtitleTimestampUnknown;
	int64_t duration_ns = 0;
	uint32_t flags = SecondarySubtitlePacketFlagNone;
	std::string payload;
};

struct SecondarySubtitlePacketStream {
	std::string codec_id;
	std::string codec_private;
	int fallback_canvas_width = 0;
	int fallback_canvas_height = 0;
	std::vector<SecondarySubtitlePacket> packets;
};
