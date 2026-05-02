// Copyright (c) 2026

#pragma once

#include <libaegisub/string_utils.h>

#include <cstdint>
#include <limits>

namespace AssCompat {

inline bool digit_value(char c, unsigned base, std::uint32_t& value) {
	if (c >= '0' && c <= '9')
		value = static_cast<std::uint32_t>(c - '0');
	else if (c >= 'a' && c <= 'f')
		value = static_cast<std::uint32_t>(c - 'a' + 10);
	else if (c >= 'A' && c <= 'F')
		value = static_cast<std::uint32_t>(c - 'A' + 10);
	else
		return false;

	return value < base;
}

inline bool ParseInteger(agi::util::strings::view text, int& out) {
	auto p = text.data();
	auto end = p + text.size();

	while (p != end && agi::util::strings::is_space(*p))
		++p;

	unsigned base = 10;
	if (end - p >= 2 && p[0] == '&' && agi::util::strings::ascii_iequals(p[1], 'h')) {
		p += 2;
		base = 16;
	}
	else if (end - p >= 2 && p[0] == '0' && agi::util::strings::ascii_iequals(p[1], 'x')) {
		p += 2;
		base = 16;
	}

	while (p != end && agi::util::strings::is_space(*p))
		++p;

	bool negative = false;
	if (p != end && (*p == '+' || *p == '-'))
		negative = *p++ == '-';

	std::uint32_t value = 0;
	std::uint32_t digit = 0;
	bool any = false;
	while (p != end && digit_value(*p, base, digit)) {
		value = value * base + digit;
		any = true;
		++p;
	}

	if (!any)
		return false;

	if (negative)
		value = 0 - value;

	std::int64_t signed_value = value;
	if (signed_value > std::numeric_limits<std::int32_t>::max())
		signed_value -= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
	out = static_cast<int>(signed_value);
	return true;
}

}
