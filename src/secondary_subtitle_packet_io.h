#pragma once

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <iterator>
#include <string>
#include <string_view>

namespace secondary_subtitle_packet_io {

inline uint16_t ReadBigEndian16(std::string_view data, size_t offset) {
	return static_cast<uint16_t>(
		(static_cast<uint16_t>(static_cast<unsigned char>(data[offset])) << 8)
		| static_cast<uint16_t>(static_cast<unsigned char>(data[offset + 1])));
}

inline uint32_t ReadBigEndian32(std::string_view data, size_t offset) {
	return (static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) << 24)
		| (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16)
		| (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8)
		| static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

inline std::string ReadFile(agi::fs::path const& filename) {
	auto input = agi::io::Open(filename, true);
	std::string data((std::istreambuf_iterator<char>(*input)), std::istreambuf_iterator<char>());
	if (input->bad())
		throw agi::io::IOFatal("Failed while reading " + agi::fs::PathToString(filename));
	return data;
}

} // namespace secondary_subtitle_packet_io
