// Copyright (c) 2026, Aegisub Project
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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class MkvTextSubtitleCodec {
	Unsupported,
	Ass,
	Ssa,
	Utf8,
};

struct MkvTextSubtitleLine {
	int sort_key = 0;
	std::string line;
};

MkvTextSubtitleCodec ClassifyMkvTextSubtitleCodec(std::string_view codec_id);
bool IsSupportedMkvTextSubtitleCodec(std::string_view codec_id);
std::vector<std::string> SplitMkvCodecPrivateLines(std::string_view codec_private);
std::optional<MkvTextSubtitleLine> ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec codec, std::string_view packet, int start_ms, int end_ms, int fallback_sort_key);
