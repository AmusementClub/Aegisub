// Copyright (c) 2026

#pragma once

#include <libaegisub/string_utils.h>

#include <charconv>
#include <cstdint>
#include <limits>

namespace AssCompat {

inline agi::util::strings::view StripStyleCompatibilityPrefix(agi::util::strings::view name) {
	while (!name.empty() && name.front() == '*')
		name.remove_prefix(1);
	return name;
}

inline bool StyleNamesMatch(agi::util::strings::view defined_name, agi::util::strings::view lookup_name) {
	auto defined = StripStyleCompatibilityPrefix(defined_name);
	auto lookup = StripStyleCompatibilityPrefix(lookup_name);

	if (agi::util::strings::iequals(lookup, "Default"))
		lookup = "Default";

	return defined == lookup;
}

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

inline bool ParseFloat(agi::util::strings::view text, double& out) {
	auto p = text.data();
	auto end = p + text.size();

	while (p != end && agi::util::strings::is_space(*p))
		++p;
	if (p != end && *p == '+')
		++p;

	auto result = std::from_chars(p, end, out);
	return result.ec == std::errc() && result.ptr != p;
}

inline bool ParseDecimalInteger(agi::util::strings::view text, int& out) {
	auto p = text.data();
	auto end = p + text.size();

	while (p != end && agi::util::strings::is_space(*p))
		++p;

	bool negative = false;
	if (p != end && (*p == '+' || *p == '-'))
		negative = *p++ == '-';

	std::uint32_t value = 0;
	bool any = false;
	while (p != end && *p >= '0' && *p <= '9') {
		value = value * 10 + static_cast<std::uint32_t>(*p - '0');
		any = true;
		++p;
	}

	if (!any)
		return false;
	while (p != end && agi::util::strings::is_space(*p))
		++p;
	if (p != end)
		return false;

	if (negative)
		value = 0 - value;

	std::int64_t signed_value = value;
	if (signed_value > std::numeric_limits<std::int32_t>::max())
		signed_value -= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
	out = static_cast<int>(signed_value);
	return true;
}

inline bool ParseTime(agi::util::strings::view text, int& out) {
	int parts[4] = {0, 0, 0, 0};
	char const separators[3] = {':', ':', '.'};
	std::size_t start = 0;

	for (int i = 0; i < 3; ++i) {
		auto end = agi::util::strings::find(text, separators[i], start);
		if (end == agi::util::strings::npos)
			return false;
		if (!ParseDecimalInteger(text.substr(start, end - start), parts[i]))
			return false;
		start = end + 1;
	}

	auto fraction = text.substr(start);
	if (!ParseDecimalInteger(fraction, parts[3]))
		return false;

	auto count_fraction_digits = [](agi::util::strings::view value) {
		auto p = value.data();
		auto end = p + value.size();
		while (p != end && agi::util::strings::is_space(*p))
			++p;
		if (p != end && (*p == '+' || *p == '-'))
			++p;

		std::size_t digits = 0;
		while (p != end && *p >= '0' && *p <= '9') {
			++digits;
			++p;
		}
		return digits;
	};

	int fractional_ms = count_fraction_digits(fraction) == 3 ? parts[3] : parts[3] * 10;

	auto milliseconds = ((static_cast<long long>(parts[0]) * 60 + parts[1]) * 60 + parts[2]) * 1000 + fractional_ms;
	if (milliseconds < 0)
		milliseconds = 0;
	if (milliseconds > std::numeric_limits<int>::max())
		milliseconds = std::numeric_limits<int>::max();

	out = static_cast<int>(milliseconds);
	return true;
}

}
