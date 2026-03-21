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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "mkv_wrap_common.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/format.h>

#include <algorithm>
#include <charconv>

namespace {
bool parse_int(std::string_view text, int &value) {
	if (text.empty())
		return false;

	auto const *begin = text.data();
	auto const *end = text.data() + text.size();
	auto [ptr, ec] = std::from_chars(begin, end, value);
	return ec == std::errc() && ptr == end;
}

std::string escape_srt_payload(std::string_view payload) {
	std::string escaped;
	escaped.reserve(payload.size());

	for (size_t i = 0; i < payload.size(); ++i) {
		auto const ch = payload[i];
		if (ch == '\r') {
			if (i + 1 < payload.size() && payload[i + 1] == '\n')
				++i;
			escaped += "\\N";
		}
		else if (ch == '\n') {
			escaped += "\\N";
		}
		else {
			escaped.push_back(ch);
		}
	}

	return escaped;
}
}

MkvTextSubtitleCodec ClassifyMkvTextSubtitleCodec(std::string_view codec_id) {
	if (codec_id == "S_TEXT/ASS")
		return MkvTextSubtitleCodec::Ass;
	if (codec_id == "S_TEXT/SSA")
		return MkvTextSubtitleCodec::Ssa;
	if (codec_id == "S_TEXT/UTF8")
		return MkvTextSubtitleCodec::Utf8;
	return MkvTextSubtitleCodec::Unsupported;
}

bool IsSupportedMkvTextSubtitleCodec(std::string_view codec_id) {
	return ClassifyMkvTextSubtitleCodec(codec_id) != MkvTextSubtitleCodec::Unsupported;
}

std::vector<std::string> SplitMkvCodecPrivateLines(std::string_view codec_private) {
	std::vector<std::string> lines;

	for (size_t pos = 0; pos < codec_private.size();) {
		while (pos < codec_private.size() && (codec_private[pos] == '\r' || codec_private[pos] == '\n'))
			++pos;

		auto end = pos;
		while (end < codec_private.size() && codec_private[end] != '\r' && codec_private[end] != '\n')
			++end;

		if (end > pos)
			lines.emplace_back(codec_private.substr(pos, end - pos));

		pos = end;
	}

	return lines;
}

std::optional<MkvTextSubtitleLine> ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec codec, std::string_view packet, int start_ms, int end_ms, int fallback_sort_key) {
	if (end_ms < start_ms)
		end_ms = start_ms;

	agi::Time const start(start_ms);
	agi::Time const end(end_ms);

	switch (codec) {
	case MkvTextSubtitleCodec::Ass:
	case MkvTextSubtitleCodec::Ssa: {
		auto const first = std::find(packet.begin(), packet.end(), ',');
		if (first == packet.end())
			return std::nullopt;

		auto const second = std::find(first + 1, packet.end(), ',');
		if (second == packet.end())
			return std::nullopt;

		int read_order = 0;
		int layer_or_marked = 0;
		if (!parse_int(std::string_view(packet.data(), static_cast<size_t>(first - packet.begin())), read_order))
			return std::nullopt;
		if (!parse_int(std::string_view(&*std::next(first), static_cast<size_t>(second - first - 1)), layer_or_marked))
			return std::nullopt;

		auto const payload = std::string_view(&*std::next(second), static_cast<size_t>(packet.end() - std::next(second)));
		return MkvTextSubtitleLine{
			read_order,
			agi::format("Dialogue: %d,%s,%s,%s",
				layer_or_marked,
				start.GetAssFormatted(),
				end.GetAssFormatted(),
				std::string(payload))
		};
	}
	case MkvTextSubtitleCodec::Utf8:
		return MkvTextSubtitleLine{
			fallback_sort_key,
			agi::format("Dialogue: 0,%s,%s,Default,,0,0,0,,%s",
				start.GetAssFormatted(),
				end.GetAssFormatted(),
				escape_srt_payload(packet))
		};
	case MkvTextSubtitleCodec::Unsupported:
		break;
	}

	return std::nullopt;
}
