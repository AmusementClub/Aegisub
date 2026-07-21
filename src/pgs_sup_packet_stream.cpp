#include "pgs_sup_packet_stream.h"

#include "secondary_subtitle_packet_io.h"

#include <libaegisub/fs.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace {
constexpr uint16_t kSupMagic = 0x5047;
constexpr int64_t kPgsClockHz = 90000;
constexpr int64_t kNanosecondsPerSecond = 1000000000;
constexpr size_t kSupHeaderSize = 10;
constexpr size_t kPgsSegmentHeaderSize = 3;

class TimestampUnwrapper {
	uint64_t epoch = 0;
	uint32_t previous = 0;
	bool have_previous = false;

public:
	uint64_t Unwrap(uint32_t value) {
		if (have_previous && value < previous
			&& static_cast<uint32_t>(previous - value) > 0x80000000u) {
			epoch += uint64_t{1} << 32;
		}
		previous = value;
		have_previous = true;
		return epoch + value;
	}
};

[[noreturn]] void ThrowTruncated(size_t offset) {
	throw PgsSupParseError("Truncated SUP segment at byte " + std::to_string(offset) + ".");
}
}

int64_t pgs_sup::TicksToNanoseconds(uint64_t ticks) {
	auto const seconds = ticks / kPgsClockHz;
	auto const remainder = ticks % kPgsClockHz;
	auto const max_ns = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	auto const max_seconds = max_ns / kNanosecondsPerSecond;
	if (seconds > max_seconds)
		return std::numeric_limits<int64_t>::max();

	auto const whole_seconds_ns = seconds * kNanosecondsPerSecond;
	auto const remainder_ns = (remainder * kNanosecondsPerSecond + kPgsClockHz / 2) / kPgsClockHz;
	if (remainder_ns > max_ns - whole_seconds_ns)
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(whole_seconds_ns + remainder_ns);
}

bool IsPgsSupSubtitlePath(agi::fs::path const& path) {
	return agi::fs::HasExtension(path, "sup") || agi::fs::HasExtension(path, "pgs");
}

SecondarySubtitlePacketStream ParsePgsSupPacketStream(std::string_view data) {
	SecondarySubtitlePacketStream stream;
	stream.codec_id = kSecondarySubtitleCodecHdmvPgs;
	TimestampUnwrapper pts_unwrapper;
	TimestampUnwrapper dts_unwrapper;

	for (size_t offset = 0; offset < data.size();) {
		if (data.size() - offset < kSupHeaderSize + kPgsSegmentHeaderSize)
			ThrowTruncated(offset);
		if (secondary_subtitle_packet_io::ReadBigEndian16(data, offset) != kSupMagic)
			throw PgsSupParseError("Invalid SUP magic at byte " + std::to_string(offset) + ".");

		auto const pts = secondary_subtitle_packet_io::ReadBigEndian32(data, offset + 2);
		auto const dts = secondary_subtitle_packet_io::ReadBigEndian32(data, offset + 6);
		auto const segment_size = static_cast<size_t>(secondary_subtitle_packet_io::ReadBigEndian16(data, offset + kSupHeaderSize + 1));
		auto const packet_size = kSupHeaderSize + kPgsSegmentHeaderSize + segment_size;
		if (packet_size < kSupHeaderSize + kPgsSegmentHeaderSize || data.size() - offset < packet_size)
			ThrowTruncated(offset);

		SecondarySubtitlePacket packet;
		packet.pts_ns = pgs_sup::TicksToNanoseconds(pts_unwrapper.Unwrap(pts));
		if (dts != 0 || pts == 0)
			packet.dts_ns = pgs_sup::TicksToNanoseconds(dts_unwrapper.Unwrap(dts));
		packet.payload.assign(data.substr(offset + kSupHeaderSize, kPgsSegmentHeaderSize + segment_size));
		stream.packets.emplace_back(std::move(packet));
		offset += packet_size;
	}

	if (stream.packets.empty())
		throw PgsSupParseError("SUP file contains no PGS segments.");
	return stream;
}

SecondarySubtitlePacketStream ReadPgsSupPacketStream(agi::fs::path const& filename) {
	return ParsePgsSupPacketStream(secondary_subtitle_packet_io::ReadFile(filename));
}
