// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#include "font_file_lister.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "font_collector_unicode.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace {
void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}
}

FontCollector::FontCollector(FontCollectorEventSink event_sink)
: event_sink(std::move(event_sink))
, lister(this->event_sink)
{
}

void FontCollector::ProcessDialogueLine(const AssDialogue *line, int index, int wrap_style) {
	if (line->Comment) return;

	auto style_it = styles.find(line->Style);
	if (style_it == end(styles)) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::StyleMissing;
		event.style = line->Style;
		Emit(event_sink, std::move(event));
		++missing;
		return;
	}

	StyleInfo style = style_it->second;
	StyleInfo initial = style;

	bool overriden = false;

	for (auto& block : line->ParseTags()) {
		switch (block->GetType()) {
		case AssBlockType::OVERRIDE:
			for (auto const& tag : static_cast<AssDialogueBlockOverride&>(*block).Tags) {
				if (tag.Name == "\\r") {
					style = styles[tag.Params[0].Get(line->Style.get())];
					overriden = false;
				}
				else if (tag.Name == "\\b") {
					style.bold = tag.Params[0].Get(initial.bold);
					overriden = true;
				}
				else if (tag.Name == "\\i") {
					style.italic = tag.Params[0].Get(initial.italic);
					overriden = true;
				}
				else if (tag.Name == "\\fn") {
					style.facename = tag.Params[0].Get(initial.facename);
					overriden = true;
				}
			}
			break;
		case AssBlockType::PLAIN: {
			auto text = block->GetText();

			if (text.empty())
				continue;

			auto& usage = used_styles[style];

			if (overriden) {
				auto& lines = usage.lines;
				if (lines.empty() || lines.back() != index)
					lines.push_back(index);
			}

			auto& chars = usage.chars;
			auto const *data = text.data();
			auto const size = text.size();
			for (size_t i = 0; i < size; ) {
				if (text[i] == '\\' && i + 1 < size) {
					char next = text[++i];
					if (next == 'N' || next == 'n') {
						++i;
						if (next == 'n' && wrap_style != 2)
							chars.push_back(0x20);
						continue;
					}
					if (next == 'h') {
						++i;
						chars.push_back(0xA0);
						continue;
					}

					chars.push_back('\\');
					continue;
				}

				auto const ch = static_cast<unsigned char>(data[i]);
				if (ch < 0x80) {
					chars.push_back(ch);
					++i;
					continue;
				}

				font_collector::unicode::Rune rune;
				int bytes_consumed = 0;
				font_collector::unicode::Rune::DecodeFromUtf8(
					data + i, size - i, rune, bytes_consumed);
				if (bytes_consumed == 0)
					bytes_consumed = 1;
				i += bytes_consumed;
				chars.push_back(rune.Value());
			}

			sort(begin(chars), end(chars));
			chars.erase(unique(chars.begin(), chars.end()), chars.end());
			break;
		}
		case AssBlockType::DRAWING:
		case AssBlockType::COMMENT:
			break;
		}
	}
}

void FontCollector::ProcessChunk(std::pair<StyleInfo, UsageData> const& style, FontCollectorDetails *details) {
	if (style.second.chars.empty()) return;

	auto res = lister.GetFontPaths(style.first.facename, style.first.bold, style.first.italic, style.second.chars);
	for (auto& path : res.paths)
		path.make_preferred();

	if (details) {
		auto& usage = details->fonts.emplace_back();
		usage.ass_facename = style.first.facename;
		usage.ass_bold = style.first.bold;
		usage.ass_italic = style.first.italic;
		usage.chars = style.second.chars;
		usage.styles = style.second.styles;
		usage.override_lines = style.second.lines;
		usage.matched.facename = res.matched_facename;
		usage.matched.face_index = res.face_index;
		usage.matched.weight = res.matched_weight;
		usage.matched.italic = res.matched_italic;
		usage.matched.is_collection = res.is_collection;
		usage.matched.paths = res.paths;
		usage.matched.path_source = res.path_source;
		usage.matched.raw_data = res.raw_data;
		usage.matched.fake_bold = res.fake_bold;
		usage.matched.fake_italic = res.fake_italic;
		usage.matched.missing_chars = res.missing;
	}

	if (res.paths.empty() && res.raw_data.bytes.empty()) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::FontMissing;
		event.face = style.first.facename;
		Emit(event_sink, std::move(event));
		PrintUsage(style.second);
		++missing;
	}
	else {
		for (auto& elem : res.paths) {
			if (std::find(begin(results), end(results), elem) == end(results)) {
				FontCollectorEvent event;
				event.type = FontCollectorEventType::FontFound;
				event.face = style.first.facename;
				event.path = elem;
				event.message = res.path_source;
				Emit(event_sink, std::move(event));
				results.push_back(elem);
			}
		}

		if (res.paths.empty() && !res.raw_data.bytes.empty()) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FontFound;
			event.face = style.first.facename;
			event.message = "memory";
			Emit(event_sink, std::move(event));
		}

		if (res.fake_bold) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FakeBold;
			event.face = style.first.facename;
			Emit(event_sink, std::move(event));
		}
		if (res.fake_italic) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FakeItalic;
			event.face = style.first.facename;
			Emit(event_sink, std::move(event));
		}

		if (res.missing.size()) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::MissingGlyphs;
			event.face = style.first.facename;
			event.message = res.missing;
			int missing_count = 0;
			for (size_t pos = 0; pos < res.missing.size(); ) {
				font_collector::unicode::Rune rune;
				int consumed = 0;
				font_collector::unicode::Rune::DecodeFromUtf8(res.missing.data() + pos, res.missing.size() - pos, rune, consumed);
				if (consumed <= 0) { ++pos; continue; }
				pos += consumed;
				++missing_count;
			}
			event.count = missing_count;
			Emit(event_sink, std::move(event));
			PrintUsage(style.second);
			++missing_glyphs;
		}
		else if (res.fake_bold || res.fake_italic)
			PrintUsage(style.second);
	}
}

void FontCollector::PrintUsage(UsageData const& data) {
	FontCollectorEvent event;
	event.type = FontCollectorEventType::Usage;
	event.styles = data.styles;
	event.lines = data.lines;
	Emit(event_sink, std::move(event));
}

std::vector<agi::fs::path> FontCollector::GetFontPaths(const AssFile *file, FontCollectorDetails *details) {
	missing = 0;
	missing_glyphs = 0;
	if (details)
		details->fonts.clear();

	FontCollectorEvent event;
	event.type = FontCollectorEventType::ParsingFile;
	Emit(event_sink, std::move(event));

	for (auto const& style : file->Styles) {
		StyleInfo &info = styles[style.name];
		info.facename = style.font;
		info.bold     = style.bold;
		info.italic   = style.italic;
		used_styles[info].styles.push_back(style.name);
	}

	int wrap_style = file->GetScriptInfoAsInt("WrapStyle");
	int index = 0;
	for (auto const& diag : file->Events)
		ProcessDialogueLine(&diag, ++index, wrap_style);
	if (details)
		details->fonts.reserve(used_styles.size());

	event = FontCollectorEvent();
	event.type = FontCollectorEventType::SearchingForFontFiles;
	Emit(event_sink, std::move(event));
	for (auto const& style : used_styles) ProcessChunk(style, details);
	event = FontCollectorEvent();
	event.type = FontCollectorEventType::SearchComplete;
	Emit(event_sink, std::move(event));

	std::vector<agi::fs::path> paths;
	paths.reserve(results.size());
	paths.insert(paths.end(), results.begin(), results.end());

	event = FontCollectorEvent();
	if (missing == 0) {
		event.type = FontCollectorEventType::AllFontsFound;
		Emit(event_sink, std::move(event));
	}
	else {
		event.type = FontCollectorEventType::FontsMissing;
		event.count = missing;
		Emit(event_sink, std::move(event));
	}
	if (missing_glyphs != 0) {
		event = FontCollectorEvent();
		event.type = FontCollectorEventType::FontsMissingGlyphs;
		event.count = missing_glyphs;
		Emit(event_sink, std::move(event));
	}

	return paths;
}

bool FontCollector::StyleInfo::operator<(StyleInfo const& rgt) const {
	return std::tie(facename, bold, italic) < std::tie(rgt.facename, rgt.bold, rgt.italic);
}
