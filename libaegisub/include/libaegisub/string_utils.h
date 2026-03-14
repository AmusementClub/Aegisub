// Copyright (c) 2026

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#if defined(__cpp_lib_string_view) || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
#define AGI_HAS_STRING_VIEW 1
#else
#define AGI_HAS_STRING_VIEW 0
#endif

#if AGI_HAS_STRING_VIEW
#include <string_view>
#endif

#ifdef AEGISUB_USE_STRINGZILLA
#include <stringzilla/stringzilla.hpp>
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

#ifdef AEGISUB_USE_STRINGZILLA
	namespace sz = ashvardanian::stringzilla;

	inline sz::string_view to_backend(view value) {
		return sz::string_view(value.data(), value.size());
	}
#endif

	inline bool contains(view haystack, char needle) {
#ifdef AEGISUB_USE_STRINGZILLA
		return to_backend(haystack).contains(needle);
#else
		return std::find(haystack.data(), haystack.data() + haystack.size(), needle) != haystack.data() + haystack.size();
#endif
	}

	inline bool contains(view haystack, view needle) {
#ifdef AEGISUB_USE_STRINGZILLA
		return to_backend(haystack).contains(to_backend(needle));
#else
		if (needle.empty()) return true;
		return std::search(
			haystack.data(), haystack.data() + haystack.size(),
			needle.data(), needle.data() + needle.size()) != haystack.data() + haystack.size();
#endif
	}

	inline bool starts_with(view haystack, view prefix) {
#ifdef AEGISUB_USE_STRINGZILLA
		return to_backend(haystack).starts_with(to_backend(prefix));
#else
		return haystack.size() >= prefix.size() &&
			std::equal(prefix.data(), prefix.data() + prefix.size(), haystack.data());
#endif
	}

	inline bool ends_with(view haystack, view suffix) {
#ifdef AEGISUB_USE_STRINGZILLA
		return to_backend(haystack).ends_with(to_backend(suffix));
#else
		return haystack.size() >= suffix.size() &&
			std::equal(suffix.data(), suffix.data() + suffix.size(), haystack.data() + haystack.size() - suffix.size());
#endif
	}

} } } // namespace agi::util::strings
