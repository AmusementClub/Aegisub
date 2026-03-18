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

#include "source_frame.h"

#include <algorithm>
#include <vector>

inline bool ContainsSourceFrameFormat(
	std::vector<SourceFramePixelFormat> const& formats,
	SourceFramePixelFormat format) {
	return std::find(formats.begin(), formats.end(), format) != formats.end();
}

inline SourceFramePixelFormat SelectPreferredSourceFrameFormat(
	std::vector<SourceFramePixelFormat> preferred_formats,
	std::vector<SourceFramePixelFormat> const& available_formats,
	bool compatibility_requires_bgra8) {
	if (compatibility_requires_bgra8)
		return SourceFramePixelFormat::Bgra8;

	if (!ContainsSourceFrameFormat(preferred_formats, SourceFramePixelFormat::Bgra8))
		preferred_formats.push_back(SourceFramePixelFormat::Bgra8);

	for (auto format : preferred_formats) {
		if (ContainsSourceFrameFormat(available_formats, format))
			return format;
	}

	if (!available_formats.empty())
		return available_formats.front();

	return SourceFramePixelFormat::Bgra8;
}
