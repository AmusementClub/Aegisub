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

#include "video_renderer_backend.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace {
bool EqualsCaseInsensitive(std::string_view left, std::string_view right) {
	if (left.size() != right.size())
		return false;

	return std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
		return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
	});
}

bool MatchesAny(std::string_view value, std::initializer_list<std::string_view> aliases) {
	for (auto alias : aliases) {
		if (EqualsCaseInsensitive(value, alias))
			return true;
	}
	return false;
}
}

VideoRendererBackend ParseVideoRendererBackend(std::string_view value) {
	if (MatchesAny(value, { "libplacebo", "placebo", "placebo-gl", "libplacebo-gl" }))
		return VideoRendererBackend::PlaceboOpenGL;

	if (MatchesAny(value, { "modern", "modern-gl", "opengl", "modern-opengl" }))
		return VideoRendererBackend::OpenGL;

	return VideoRendererBackend::OpenGL;
}

std::string_view VideoRendererBackendOptionValue(VideoRendererBackend backend) {
	switch (backend) {
	case VideoRendererBackend::PlaceboOpenGL:
		return "libplacebo";
	case VideoRendererBackend::OpenGL:
	default:
		return "opengl";
	}
}

VideoRendererBackendDecision ResolveVideoRendererBackend(std::string_view value, bool placebo_available) {
	VideoRendererBackendDecision decision;
	decision.requested = ParseVideoRendererBackend(value);
	decision.actual = decision.requested;

	if (decision.requested == VideoRendererBackend::PlaceboOpenGL && !placebo_available) {
		decision.actual = VideoRendererBackend::OpenGL;
		decision.fell_back = true;
	}

	return decision;
}
