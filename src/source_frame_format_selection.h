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

inline bool ContainsSourceFrameOutputMode(
	std::vector<SourceFrameOutputMode> const& modes,
	SourceFrameOutputMode mode) {
	return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

inline SourceFrameOutputMode SelectPreferredSourceFrameOutputMode(
	std::vector<SourceFrameOutputMode> preferred_modes,
	std::vector<SourceFrameOutputMode> const& available_modes,
	bool compatibility_requires_bgra8) {
	if (compatibility_requires_bgra8)
		return SourceFrameOutputMode::Bgra8;

	if (!ContainsSourceFrameOutputMode(preferred_modes, SourceFrameOutputMode::Bgra8))
		preferred_modes.push_back(SourceFrameOutputMode::Bgra8);

	for (auto mode : preferred_modes) {
		if (ContainsSourceFrameOutputMode(available_modes, mode))
			return mode;
	}

	if (!available_modes.empty())
		return available_modes.front();

	return SourceFrameOutputMode::Bgra8;
}
