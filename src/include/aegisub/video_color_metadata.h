// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <cctype>
#include <string>
#include <string_view>

enum class SourceFrameColorRange {
	Unknown,
	Limited,
	Full
};

struct SourceFrameColorMetadata {
	std::string matrix;
	std::string primaries;
	std::string transfer;
	SourceFrameColorRange range = SourceFrameColorRange::Unknown;
};

inline std::string NormalizeSourceFrameColorToken(std::string_view value) {
	std::string normalized;
	normalized.reserve(value.size());
	for (unsigned char ch : value) {
		if (std::isalnum(ch))
			normalized.push_back(static_cast<char>(std::toupper(ch)));
	}
	return normalized;
}

inline bool ContainsSourceFrameColorToken(std::string const& value, char const *token) {
	return value.find(token) != std::string::npos;
}

inline SourceFrameColorMetadata SourceFrameColorMetadataFromLegacyColorSpace(std::string value) {
	SourceFrameColorMetadata color;
	color.matrix = std::move(value);

	auto token = NormalizeSourceFrameColorToken(color.matrix);
	if (token.empty())
		return color;

	// Current legacy providers all hand renderer-facing BGRA, so the packed RGB
	// samples should be treated as full-range even when the source YUV used TV
	// range before conversion.
	color.range = SourceFrameColorRange::Full;

	if (ContainsSourceFrameColorToken(token, "NONE") || ContainsSourceFrameColorToken(token, "RGB"))
		return color;

	if (ContainsSourceFrameColorToken(token, "2020"))
		color.primaries = "BT.2020";
	else if (ContainsSourceFrameColorToken(token, "470M"))
		color.primaries = "BT.470M";
	else if (ContainsSourceFrameColorToken(token, "470BG"))
		color.primaries = "BT.601-625";
	else if (ContainsSourceFrameColorToken(token, "170M"))
		color.primaries = "BT.601-525";
	else if (ContainsSourceFrameColorToken(token, "240M"))
		color.primaries = "SMPTE-240M";
	else if (ContainsSourceFrameColorToken(token, "601"))
		color.primaries = "BT.601";
	else if (ContainsSourceFrameColorToken(token, "709"))
		color.primaries = "BT.709";

	return color;
}

inline SourceFrameColorMetadata MergeSourceFrameColorMetadata(
	SourceFrameColorMetadata base,
	SourceFrameColorMetadata const& override_color) {
	if (!override_color.matrix.empty())
		base.matrix = override_color.matrix;
	if (!override_color.primaries.empty())
		base.primaries = override_color.primaries;
	if (!override_color.transfer.empty())
		base.transfer = override_color.transfer;
	if (override_color.range != SourceFrameColorRange::Unknown)
		base.range = override_color.range;
	return base;
}
