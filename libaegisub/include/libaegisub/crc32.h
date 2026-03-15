#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace agi::util {
namespace detail {
constexpr auto make_crc32_table() {
	std::array<uint32_t, 256> table{};
	for (uint32_t i = 0; i < table.size(); ++i) {
		uint32_t value = i;
		for (int bit = 0; bit < 8; ++bit)
			value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
		table[i] = value;
	}
	return table;
}
}

inline uint32_t crc32(std::string_view value) noexcept {
	static constexpr auto table = detail::make_crc32_table();
	uint32_t crc = 0xFFFFFFFFu;
	for (unsigned char byte : value)
		crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFFu];
	return ~crc;
}
}
