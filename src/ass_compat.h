// Copyright (c) 2026

#pragma once

#include <libaegisub/color.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace AssCompat {

struct StyleNameAnalysis {
	std::string suggested_name;
	bool has_compatibility_prefix = false;
	bool has_default_case_mismatch = false;

	bool NeedsExplicitRepair() const {
		return has_compatibility_prefix || has_default_case_mismatch;
	}
};

inline agi::util::strings::view StripStyleCompatibilityPrefix(agi::util::strings::view name) {
	while (!name.empty() && name.front() == '*')
		name.remove_prefix(1);
	return name;
}

inline StyleNameAnalysis AnalyzeStyleName(agi::util::strings::view name) {
	auto stripped = StripStyleCompatibilityPrefix(name);
	StyleNameAnalysis analysis;
	analysis.has_compatibility_prefix = stripped.size() != name.size();
	analysis.has_default_case_mismatch = agi::util::strings::iequals(stripped, "Default") && stripped != "Default";
	analysis.suggested_name = analysis.has_default_case_mismatch ? "Default" : std::string(stripped.begin(), stripped.end());
	return analysis;
}

inline std::string SuggestStyleNameRepair(agi::util::strings::view name) {
	return AnalyzeStyleName(name).suggested_name;
}

inline bool StyleNamesMatch(agi::util::strings::view defined_name, agi::util::strings::view lookup_name) {
	auto defined = StripStyleCompatibilityPrefix(defined_name);
	auto lookup = StripStyleCompatibilityPrefix(lookup_name);

	if (agi::util::strings::iequals(lookup, "Default"))
		lookup = "Default";

	return defined == lookup;
}

template <class StyleMap>
typename StyleMap::const_iterator FindStyle(StyleMap const& styles, std::string const& name) {
	auto exact = styles.find(name);
	if (exact != styles.end())
		return exact;

	for (auto it = styles.begin(); it != styles.end(); ++it) {
		if (StyleNamesMatch(it->first, name))
			return it;
	}
	return styles.end();
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

	double parsed = 0.0;
	auto result = std::from_chars(p, end, parsed);
	if (result.ec != std::errc() || result.ptr == p || !std::isfinite(parsed))
		return false;
	out = parsed;
	return true;
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

inline bool ParseStyleColor(agi::util::strings::view text, agi::Color& out) {
	int value = 0;
	if (!ParseInteger(text, value))
		return false;

	auto color = static_cast<std::uint32_t>(value);
	out = agi::Color(
		static_cast<unsigned char>(color & 0xFF),
		static_cast<unsigned char>((color >> 8) & 0xFF),
		static_cast<unsigned char>((color >> 16) & 0xFF),
		static_cast<unsigned char>((color >> 24) & 0xFF));
	return true;
}

inline bool ParseOverrideColor(agi::util::strings::view text, agi::Color& out) {
	auto p = text.data();
	auto end = p + text.size();

	while (p != end && (*p == '&' || agi::util::strings::ascii_iequals(*p, 'h')))
		++p;

	std::uint32_t value = 0;
	std::uint32_t digit = 0;
	bool any = false;
	while (p != end && digit_value(*p, 16, digit)) {
		value = value * 16 + digit;
		any = true;
		++p;
	}

	if (!any)
		return false;

	out = agi::Color(
		static_cast<unsigned char>(value & 0xFF),
		static_cast<unsigned char>((value >> 8) & 0xFF),
		static_cast<unsigned char>((value >> 16) & 0xFF),
		static_cast<unsigned char>((value >> 24) & 0xFF));
	return true;
}

inline bool ParseOverrideAlpha(agi::util::strings::view text, int& out) {
	agi::Color color;
	if (!ParseOverrideColor(text, color))
		return false;
	out = color.r;
	return true;
}

// Tag-level SSA \a → ASS \an, matching libass ass_parse.c (VSFilter quirk):
// values 1..11 are accepted, and illegal \a4 / \a8 are treated like \a5
// (top-left, \an7). Out-of-range values keep `fallback` (typically the
// event style alignment, or 0 meaning "no override").
inline int NormalizeLegacyAssAlignment(int ssa, int fallback) noexcept {
	switch (ssa) {
		case 1: return 1;
		case 2: return 2;
		case 3: return 3;
		case 4: return 7;
		case 5: return 7;
		case 6: return 8;
		case 7: return 9;
		case 8: return 7;
		case 9: return 4;
		case 10: return 5;
		case 11: return 6;
		default: return fallback;
	}
}

inline std::string FormatInteger(int value) {
	return std::to_string(value);
}

inline std::string FormatUnsignedInteger(std::uint32_t value) {
	return std::to_string(value);
}

inline std::string FormatFloat(double value) {
	if (!std::isfinite(value))
		return "0";
	if (value == 0.0)
		return "0";

	char buffer[384];
	auto result = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::fixed, 3);
	std::string text = result.ec == std::errc() ? std::string(buffer, result.ptr) : std::to_string(value);
	if (text.empty())
		return "0";

	auto dot = text.find('.');
	if (dot == std::string::npos)
		return text;

	auto pos = text.find_last_not_of('0');
	if (pos == std::string::npos)
		return "0";

	text.erase(pos == dot ? dot : pos + 1);
	if (text == "-0")
		return "0";
	return text;
}

inline void append_two_digits(std::string& text, int value) {
	text.push_back(static_cast<char>('0' + value / 10));
	text.push_back(static_cast<char>('0' + value % 10));
}

inline std::string FormatTime(int milliseconds, bool ms_precision = false) {
	milliseconds = std::max(0, milliseconds);
	int hours = milliseconds / 3600000;
	int minutes = (milliseconds / 60000) % 60;
	int seconds = (milliseconds / 1000) % 60;
	int centiseconds = (milliseconds % 1000) / 10;

	auto text = std::to_string(hours);
	text.push_back(':');
	append_two_digits(text, minutes);
	text.push_back(':');
	append_two_digits(text, seconds);
	text.push_back('.');
	append_two_digits(text, centiseconds);
	if (ms_precision)
		text += static_cast<char>('0' + milliseconds % 10);
	return text;
}

inline std::string FormatStyleColor(agi::Color const& color) {
	return color.GetAssStyleFormatted();
}

inline std::string FormatOverrideColor(agi::Color const& color) {
	return color.GetAssOverrideFormatted();
}

inline std::string FormatOverrideAlpha(int value) {
	static char const digits[] = "0123456789ABCDEF";
	auto clamped = std::clamp(value, 0, 255);
	std::string text = "&H";
	text.push_back(digits[clamped >> 4]);
	text.push_back(digits[clamped & 0xF]);
	text.push_back('&');
	return text;
}

}
