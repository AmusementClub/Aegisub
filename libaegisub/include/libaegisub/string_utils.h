// Copyright (c) 2026

#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef AEGISUB_USE_STRINGZILLA
#include <stringzilla/stringzilla.h>
#endif

namespace agi { namespace util { namespace strings {

constexpr std::size_t npos = static_cast<std::size_t>(-1);

struct sized_match {
	std::size_t offset = npos;
	std::size_t length = 0;

	explicit operator bool() const { return offset != npos; }
};

using view = std::string_view;

inline view subview(view value, std::size_t pos, std::size_t count = npos) {
	if (pos > value.size()) return view();
	return value.substr(pos, count);
}

inline char ascii_to_lower(char value) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

inline bool ascii_iequals(char left, char right) {
	return ascii_to_lower(left) == ascii_to_lower(right);
}

inline bool is_space(char value) {
	return std::isspace(static_cast<unsigned char>(value)) != 0;
}

#ifdef AEGISUB_USE_STRINGZILLA
inline char const* ascii_lower_lut() {
	static char lut[256];
	static const bool initialized = [] {
		sz_lookup_init_lower(lut);
		return true;
	}();
	(void)initialized;
	return lut;
}
#endif

inline std::size_t find(view haystack, char needle, std::size_t pos = 0) {
	if (pos >= haystack.size()) return npos;
#ifdef AEGISUB_USE_STRINGZILLA
	auto result = sz_find_byte(haystack.data() + pos, haystack.size() - pos, &needle);
	return result ? static_cast<std::size_t>(result - haystack.data()) : npos;
#else
	return haystack.find(needle, pos);
#endif
}

inline std::size_t find(view haystack, view needle, std::size_t pos = 0) {
	if (needle.empty()) return pos <= haystack.size() ? pos : npos;
	if (pos >= haystack.size()) return npos;
#ifdef AEGISUB_USE_STRINGZILLA
	auto result = sz_find(haystack.data() + pos, haystack.size() - pos, needle.data(), needle.size());
	return result ? static_cast<std::size_t>(result - haystack.data()) : npos;
#else
	return haystack.find(needle, pos);
#endif
}

inline bool contains(view haystack, char needle) {
	return find(haystack, needle) != npos;
}

inline bool contains(view haystack, view needle) {
	return find(haystack, needle) != npos;
}

inline bool starts_with(view haystack, view prefix) {
	if (haystack.size() < prefix.size()) return false;
#ifdef AEGISUB_USE_STRINGZILLA
	return sz_equal(haystack.data(), prefix.data(), prefix.size()) == sz_true_k;
#else
	return std::equal(prefix.data(), prefix.data() + prefix.size(), haystack.data());
#endif
}

inline bool ends_with(view haystack, view suffix) {
	if (haystack.size() < suffix.size()) return false;
	auto start = haystack.data() + haystack.size() - suffix.size();
#ifdef AEGISUB_USE_STRINGZILLA
	return sz_equal(start, suffix.data(), suffix.size()) == sz_true_k;
#else
	return std::equal(suffix.data(), suffix.data() + suffix.size(), start);
#endif
}

inline bool iequals(view left, view right) {
	return left.size() == right.size() &&
		std::equal(left.data(), left.data() + left.size(), right.data(), ascii_iequals);
}

inline bool istarts_with(view haystack, view prefix) {
	return haystack.size() >= prefix.size() &&
		std::equal(prefix.data(), prefix.data() + prefix.size(), haystack.data(), ascii_iequals);
}

inline bool iends_with(view haystack, view suffix) {
	return haystack.size() >= suffix.size() &&
		std::equal(suffix.data(), suffix.data() + suffix.size(), haystack.data() + haystack.size() - suffix.size(), ascii_iequals);
}

inline void to_lower_inplace(std::string& value) {
#ifdef AEGISUB_USE_STRINGZILLA
	if (!value.empty())
		sz_lookup(value.data(), value.size(), value.data(), ascii_lower_lut());
#else
	std::transform(value.begin(), value.end(), value.begin(), ascii_to_lower);
#endif
}

inline std::string to_lower_copy(view value) {
	std::string copy(value);
	to_lower_inplace(copy);
	return copy;
}

inline void trim_left_inplace(std::string& value) {
	value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
}

inline void trim_right_inplace(std::string& value) {
	value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
}

inline void trim_inplace(std::string& value) {
	trim_right_inplace(value);
	trim_left_inplace(value);
}

inline std::string trim_copy(view value) {
	std::string copy(value);
	trim_inplace(copy);
	return copy;
}

inline std::string trim_left_copy(view value) {
	std::string copy(value);
	trim_left_inplace(copy);
	return copy;
}

inline std::string trim_right_copy(view value) {
	std::string copy(value);
	trim_right_inplace(copy);
	return copy;
}

inline void replace_all_inplace(std::string& value, view needle, view replacement) {
	if (needle.empty()) return;
	std::size_t pos = 0;
	while ((pos = find(value, needle, pos)) != npos) {
		value.replace(pos, needle.size(), replacement.data(), replacement.size());
		pos += replacement.size();
	}
}

inline std::string replace_all_copy(view value, view needle, view replacement) {
	std::string copy(value);
	replace_all_inplace(copy, needle, replacement);
	return copy;
}

inline void ireplace_all_ascii_inplace(std::string& value, view needle, view replacement) {
	if (needle.empty()) return;
	for (std::size_t pos = 0; pos + needle.size() <= value.size(); ) {
		if (istarts_with(view(value.data() + pos, value.size() - pos), needle)) {
			value.replace(pos, needle.size(), replacement.data(), replacement.size());
			pos += replacement.size();
		}
		else {
			++pos;
		}
	}
}

inline void replace_range_inplace(std::string& value, std::size_t begin, std::size_t end, view replacement) {
	value.replace(begin, end - begin, replacement.data(), replacement.size());
}

template<typename Integer>
inline typename std::enable_if<std::is_integral<Integer>::value, bool>::type parse_integer(view value, Integer& out) {
	auto first = value.data();
	auto last = value.data() + value.size();
	auto result = std::from_chars(first, last, out);
	return result.ec == std::errc() && result.ptr == last;
}

inline bool parse_hex_byte(view value, unsigned char& out) {
	unsigned int tmp = 0;
	auto first = value.data();
	auto last = value.data() + value.size();
	auto result = std::from_chars(first, last, tmp, 16);
	if (result.ec != std::errc() || result.ptr != last || tmp > 0xFF)
		return false;
	out = static_cast<unsigned char>(tmp);
	return true;
}

inline bool utf8_iequals(view left, view right) {
#ifdef AEGISUB_USE_STRINGZILLA
	return sz_utf8_case_insensitive_order(left.data(), left.size(), right.data(), right.size()) == sz_equal_k;
#else
	return iequals(left, right);
#endif
}

inline sized_match utf8_find_icase(view haystack, view needle) {
#ifdef AEGISUB_USE_STRINGZILLA
	sz_utf8_case_insensitive_needle_metadata_t metadata = {};
	sz_size_t match_length = 0;
	auto ptr = sz_utf8_case_insensitive_find(haystack.data(), haystack.size(), needle.data(), needle.size(), &metadata, &match_length);
	if (!ptr) return {};
	return { static_cast<std::size_t>(ptr - haystack.data()), static_cast<std::size_t>(match_length) };
#else
	auto pos = find(haystack, needle);
	return pos == npos ? sized_match{} : sized_match{pos, needle.size()};
#endif
}

inline bool utf8_istarts_with(view haystack, view prefix) {
	auto match = utf8_find_icase(haystack, prefix);
	return match && match.offset == 0;
}

} } } // namespace agi::util::strings
