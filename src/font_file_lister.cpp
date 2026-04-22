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

#include <algorithm>
#include <tuple>
#include <unicode/utf8.h>

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

void FontCollector::ProcessDialogueLine(const AssDialogue *line, int index) {
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
			auto size = static_cast<int>(text.size());
			for (int i = 0; i < size; ) {
				if (text[i] == '\\' && i + 1 < size) {
					char next = text[++i];
					if (next == 'N' || next == 'n') {
						++i;
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

				UChar32 c;
				U8_NEXT(&text[0], i, size, c);
				chars.push_back(c);
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

void FontCollector::ProcessChunk(std::pair<StyleInfo, UsageData> const& style) {
	if (style.second.chars.empty()) return;

	auto res = lister.GetFontPaths(style.first.facename, style.first.bold, style.first.italic, style.second.chars);

	if (res.paths.empty()) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::FontMissing;
		event.face = style.first.facename;
		Emit(event_sink, std::move(event));
		PrintUsage(style.second);
		++missing;
	}
	else {
		for (auto& elem : res.paths) {
			elem.make_preferred();
			if (std::find(begin(results), end(results), elem) == end(results)) {
				FontCollectorEvent event;
				event.type = FontCollectorEventType::FontFound;
				event.face = style.first.facename;
				event.path = elem;
				Emit(event_sink, std::move(event));
				results.push_back(elem);
			}
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
			event.count = static_cast<int>(res.missing.size());
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

std::vector<agi::fs::path> FontCollector::GetFontPaths(const AssFile *file) {
	missing = 0;
	missing_glyphs = 0;

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

	int index = 0;
	for (auto const& diag : file->Events)
		ProcessDialogueLine(&diag, ++index);

	event = FontCollectorEvent();
	event.type = FontCollectorEventType::SearchingForFontFiles;
	Emit(event_sink, std::move(event));
	for (auto const& style : used_styles) ProcessChunk(style);
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
