// Copyright (c) 2026

#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>

#if defined(__cpp_lib_string_view) || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
#define AGI_HAS_STRING_VIEW 1
#else
#define AGI_HAS_STRING_VIEW 0
#endif

#if AGI_HAS_STRING_VIEW
#include <string_view>
#endif

#ifdef AEGISUB_USE_STRINGZILLA
#include <stringzilla/stringzilla.h>
#endif

namespace agi { namespace util { namespace strings {

class view {
	const char *ptr = "";
	std::size_t len = 0;

public:
	view() = default;
	view(const char *data, std::size_t size) : ptr(data ? data : ""), len(data ? size : 0) { }
	view(const char *value) : ptr(value ? value : ""), len(value ? std::strlen(value) : 0) { }
	view(std::string const& value) : ptr(value.data()), len(value.size()) { }
#if AGI_HAS_STRING_VIEW
	view(std::string_view const& value) : ptr(value.data()), len(value.size()) { }
#endif

	const char *data() const { return ptr; }
	std::size_t size() const { return len; }
	bool empty() const { return len == 0; }
};

inline char ascii_to_lower(char value) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

inline bool ascii_iequals(char left, char right) {
	return ascii_to_lower(left) == ascii_to_lower(right);
}

inline bool is_space(char value) {
	return std::isspace(static_cast<unsigned char>(value)) != 0;
}

	inline bool contains(view haystack, char needle) {
#ifdef AEGISUB_USE_STRINGZILLA
		return sz_find_byte(haystack.data(), haystack.size(), &needle) != nullptr;
#else
		return std::find(haystack.data(), haystack.data() + haystack.size(), needle) != haystack.data() + haystack.size();
#endif
	}

	inline bool contains(view haystack, view needle) {
#ifdef AEGISUB_USE_STRINGZILLA
		return needle.empty() || sz_find(haystack.data(), haystack.size(), needle.data(), needle.size()) != nullptr;
#else
		if (needle.empty()) return true;
		return std::search(
			haystack.data(), haystack.data() + haystack.size(),
			needle.data(), needle.data() + needle.size()) != haystack.data() + haystack.size();
#endif
	}

	inline bool starts_with(view haystack, view prefix) {
		return haystack.size() >= prefix.size() &&
			std::equal(prefix.data(), prefix.data() + prefix.size(), haystack.data());
	}

	inline bool ends_with(view haystack, view suffix) {
		return haystack.size() >= suffix.size() &&
			std::equal(suffix.data(), suffix.data() + suffix.size(), haystack.data() + haystack.size() - suffix.size());
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
		std::transform(value.begin(), value.end(), value.begin(), ascii_to_lower);
	}

	inline std::string to_lower_copy(view value) {
		std::string copy(value.data(), value.size());
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
		std::string copy(value.data(), value.size());
		trim_inplace(copy);
		return copy;
	}

	inline std::string trim_left_copy(view value) {
		std::string copy(value.data(), value.size());
		trim_left_inplace(copy);
		return copy;
	}

	inline std::string trim_right_copy(view value) {
		std::string copy(value.data(), value.size());
		trim_right_inplace(copy);
		return copy;
	}

	inline void replace_all_inplace(std::string& value, view needle, view replacement) {
		if (needle.empty()) return;
		std::size_t pos = 0;
		while ((pos = value.find(std::string(needle.data(), needle.size()), pos)) != std::string::npos) {
			value.replace(pos, needle.size(), replacement.data(), replacement.size());
			pos += replacement.size();
		}
	}

	inline std::string replace_all_copy(view value, view needle, view replacement) {
		std::string copy(value.data(), value.size());
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

	template<typename Integer>
	inline typename std::enable_if<std::is_integral<Integer>::value, bool>::type parse_integer(view value, Integer& out) {
		auto first = value.data();
		auto last = value.data() + value.size();
		auto result = std::from_chars(first, last, out);
		return result.ec == std::errc() && result.ptr == last;
	}

} } } // namespace agi::util::strings
