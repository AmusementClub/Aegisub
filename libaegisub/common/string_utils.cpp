#include "libaegisub/string_utils.h"

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace {

struct decoded_codepoint {
	char32_t value;
	size_t length;
};

std::optional<decoded_codepoint> decode_first(std::string_view text) {
	int32_t offset = 0;
	int32_t const length = static_cast<int32_t>(std::min<size_t>(text.size(), U8_MAX_LENGTH));
	UChar32 codepoint;
	U8_NEXT(reinterpret_cast<uint8_t const *>(text.data()), offset, length, codepoint);
	if (codepoint < 0)
		return std::nullopt;
	return decoded_codepoint{static_cast<char32_t>(codepoint), static_cast<size_t>(offset)};
}

std::optional<decoded_codepoint> decode_last(std::string_view text) {
	size_t const window_start = text.size() > U8_MAX_LENGTH ? text.size() - U8_MAX_LENGTH : 0;
	auto const window = text.substr(window_start);
	int32_t offset = static_cast<int32_t>(window.size());
	UChar32 codepoint;
	U8_PREV(reinterpret_cast<uint8_t const *>(window.data()), 0, offset, codepoint);
	if (codepoint < 0)
		return std::nullopt;
	return decoded_codepoint{static_cast<char32_t>(codepoint), window.size() - static_cast<size_t>(offset)};
}

std::pair<size_t, size_t> trim_utf8_bounds(std::string_view text, bool trim_left, bool trim_right) {
	size_t begin = 0;
	size_t end = text.size();

	if (trim_left) {
		while (begin < end) {
			auto codepoint = decode_first(text.substr(begin, end - begin));
			if (!codepoint || !agi::util::strings::is_unicode_trim_character(codepoint->value))
				break;
			begin += codepoint->length;
		}
	}

	if (trim_right) {
		while (begin < end) {
			auto codepoint = decode_last(text.substr(begin, end - begin));
			if (!codepoint || !agi::util::strings::is_unicode_trim_character(codepoint->value))
				break;
			end -= codepoint->length;
		}
	}

	return {begin, end};
}

std::string trim_utf8(std::string_view text, bool trim_left, bool trim_right) {
	auto const [begin, end] = trim_utf8_bounds(text, trim_left, trim_right);
	return std::string(text.substr(begin, end - begin));
}

}

namespace agi::util::strings {

bool is_unicode_trim_character(char32_t codepoint) {
	return codepoint == 0xFEFF
		|| (is_valid_unicode_scalar(codepoint) && u_isUWhiteSpace(static_cast<UChar32>(codepoint)));
}

std::string trim_utf8_left_copy(view value) {
	return trim_utf8(value, true, false);
}

std::string trim_utf8_right_copy(view value) {
	return trim_utf8(value, false, true);
}

std::string trim_utf8_copy(view value) {
	return trim_utf8(value, true, true);
}

}
