// Shared Windows-only helpers for observation observe / derive / repository.
// Not a public product API — keep implementations in this header (header-only)
// so the three translation units stay free of ODR-duplicated anonymous helpers.
#pragma once

#ifndef _WIN32
#error "font_family_obs_internal_win.h is Windows-only"
#endif

#include <libaegisub/charset_conv_win.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace font_family_obs_internal {

inline std::string ascii_lower(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (unsigned char ch : text)
		result.push_back(static_cast<char>(std::tolower(ch)));
	return result;
}

inline std::optional<std::wstring> to_utf16(std::string_view value) {
	if (value.empty())
		return std::wstring{};
	try {
		return agi::charset::ConvertW(std::string(value));
	}
	catch (...) {
		return std::nullopt;
	}
}

inline bool ordinal_icase_equal(std::string_view left, std::string_view right) {
	auto const left_wide = to_utf16(left);
	auto const right_wide = to_utf16(right);
	return left_wide && right_wide &&
		CompareStringOrdinal(
			left_wide->data(), static_cast<int>(left_wide->size()),
			right_wide->data(), static_cast<int>(right_wide->size()), TRUE) == CSTR_EQUAL;
}

// ordinal_icase_less stays in font_family_obs_repository_win.cpp (only M0
// family-name sort needs it). Do not re-export it here.

} // namespace font_family_obs_internal
