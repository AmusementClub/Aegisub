// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
// Copyright (c) 2026, MIRIMIRIM
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

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "ass_style_resolution.h"
#include "font_collector_unicode.h"
#include "font_matching_common.h"

#include <algorithm>
#include <iterator>
#include <tuple>
#include <utility>

namespace {
constexpr size_t PendingCodepointFlushThreshold = 4096;

void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}

std::string_view PlainTextView(AssDialogueBlock& block) {
	return static_cast<AssDialogueBlockPlain&>(block).text;
}

template<class Callback>
bool ForEachAssTextCodepoint(std::string_view text, int wrap_style, Callback&& callback) {
	auto const *data = text.data();
	auto const size = text.size();
	for (size_t i = 0; i < size; ) {
		uint32_t value = 0;
		if (text[i] == '\\' && i + 1 < size) {
			char next = text[++i];
			if (next == 'N' || next == 'n') {
				++i;
				if (next == 'n' && wrap_style != 2)
					value = 0x20;
				else
					continue;
			}
			else if (next == 'h') {
				++i;
				value = 0xA0;
			}
			else {
				value = '\\';
			}
		}
		else {
			auto const ch = static_cast<unsigned char>(data[i]);
			if (ch < 0x80) {
				value = ch;
				++i;
			}
			else {
				font_collector::unicode::Rune rune;
				int bytes_consumed = 0;
				font_collector::unicode::Rune::DecodeFromUtf8(
					data + i, size - i, rune, bytes_consumed);
				if (bytes_consumed == 0)
					bytes_consumed = 1;
				i += bytes_consumed;
				value = rune.Value();
			}
		}

		if (callback(value))
			return true;
	}

	return false;
}

bool TextContainsAnyCodepoint(std::string_view text, int wrap_style, std::vector<uint32_t> const& codepoints) {
	return ForEachAssTextCodepoint(text, wrap_style, [&](uint32_t value) {
		return std::binary_search(codepoints.begin(), codepoints.end(), value);
	});
}

std::vector<uint32_t> DecodeUniqueUtf8Codepoints(std::string_view text) {
	std::vector<uint32_t> codepoints;
	for (size_t pos = 0; pos < text.size(); ) {
		font_collector::unicode::Rune rune;
		int consumed = 0;
		font_collector::unicode::Rune::DecodeFromUtf8(text.data() + pos, text.size() - pos, rune, consumed);
		if (consumed <= 0) {
			++pos;
			continue;
		}
		pos += consumed;
		codepoints.push_back(rune.Value());
	}

	sort(begin(codepoints), end(codepoints));
	codepoints.erase(unique(codepoints.begin(), codepoints.end()), codepoints.end());
	return codepoints;
}

std::unique_ptr<IFontFileLister> CreateDefaultFontFileLister(FontCollectorEventSink& event_sink) {
	return std::make_unique<FontFileLister>(event_sink);
}
}

FontCollector::FontCollector(FontCollectorEventSink event_sink)
: event_sink(std::move(event_sink))
, lister(CreateDefaultFontFileLister(this->event_sink))
{
}

FontCollector::FontCollector(FontCollectorEventSink event_sink, std::unique_ptr<IFontFileLister> lister)
: event_sink(std::move(event_sink))
, lister(std::move(lister))
{
}

FontCollector::StyleInfo FontCollector::MakeStyleInfo(AssStyle const& style) const {
	StyleInfo info;
	info.facename = style.font;
	info.bold = style.bold;
	info.italic = style.italic;
	return info;
}

void FontCollector::RecordMissingStyle(std::string const& name, int line_index) {
	auto& lines = missing_style_lines[name];
	if (line_index > 0 && (lines.empty() || lines.back() != line_index))
		lines.push_back(line_index);

	if (line_index <= 0 && lines.empty())
		lines.push_back(line_index);
}

void FontCollector::EmitMissingStyles() {
	for (auto const& [name, lines] : missing_style_lines) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::StyleMissing;
		event.style = name;
		event.lines.reserve(lines.size());
		for (int line : lines) {
			if (line > 0)
				event.lines.push_back(line);
		}
		Emit(event_sink, std::move(event));
		++missing;
	}
}

template<class Callback>
bool FontCollector::ForEachLineTextSpan(AssDialogue const& line, int line_index, int wrap_style, Callback&& callback, bool report_missing_styles) {
	auto *initial_style = aegisub::ass_style_resolution::ResolveEventStyle(*ass_file, line.Style);
	if (!initial_style) {
		if (report_missing_styles)
			RecordMissingStyle(line.Style, line_index);
		return false;
	}

	StyleInfo style = MakeStyleInfo(*initial_style);
	StyleInfo initial = style;
	bool style_valid = true;

	bool overriden = false;

	for (auto& block : line.ParseTags()) {
		switch (block->GetType()) {
		case AssBlockType::OVERRIDE:
			for (auto const& tag : static_cast<AssDialogueBlockOverride&>(*block).Tags) {
				if (tag.Name == "\\r") {
					auto const& param = tag.Params[0];
					if (param.omitted || param.empty) {
						style = initial;
						style_valid = true;
						overriden = false;
					}
					else {
						auto style_name = param.Get<std::string>();
						auto *reset_style = aegisub::ass_style_resolution::ResolveResetStyle(*ass_file, style_name);
						if (reset_style) {
							style = MakeStyleInfo(*reset_style);
							style_valid = true;
							overriden = false;
						}
						else {
							style_valid = false;
							overriden = false;
							if (report_missing_styles)
								RecordMissingStyle(style_name, line_index);
						}
					}
				}
				else if (tag.Name == "\\b") {
					if (!style_valid)
						continue;
					style.bold = tag.Params[0].Get(initial.bold);
					overriden = true;
				}
				else if (tag.Name == "\\i") {
					if (!style_valid)
						continue;
					style.italic = tag.Params[0].Get(initial.italic);
					overriden = true;
				}
				else if (tag.Name == "\\fn") {
					if (!style_valid)
						continue;
					style.facename = tag.Params[0].Get(initial.facename);
					overriden = true;
				}
			}
			break;
		case AssBlockType::PLAIN: {
			auto text = PlainTextView(*block);

			if (text.empty() || !style_valid)
				continue;

			if (callback(style, overriden, text))
				return true;
			break;
		}
		case AssBlockType::DRAWING:
		case AssBlockType::COMMENT:
			break;
		}
	}

	return true;
}

void FontCollector::AddCodepoint(UsageData& data, uint32_t codepoint) {
	if (codepoint < 256) {
		data.latin1_codepoints[codepoint / 64] |= uint64_t(1) << (codepoint % 64);
		return;
	}

	data.pending_codepoints.push_back(codepoint);
	if (data.pending_codepoints.size() >= PendingCodepointFlushThreshold)
		MergePendingCodepoints(data);
}

void FontCollector::AppendTextCodepoints(std::string_view text, int wrap_style, UsageData& data) {
	ForEachAssTextCodepoint(text, wrap_style, [&](uint32_t value) {
		AddCodepoint(data, value);
		return false;
	});
}

void FontCollector::MergePendingCodepoints(UsageData& data) {
	if (data.pending_codepoints.empty())
		return;

	auto& pending = data.pending_codepoints;
	sort(begin(pending), end(pending));
	pending.erase(unique(begin(pending), end(pending)), end(pending));

	if (data.codepoints.empty()) {
		data.codepoints.swap(pending);
		pending.clear();
		return;
	}

	std::vector<uint32_t> merged;
	merged.reserve(data.codepoints.size() + pending.size());
	std::set_union(begin(data.codepoints), end(data.codepoints), begin(pending), end(pending), std::back_inserter(merged));
	data.codepoints.swap(merged);
	pending.clear();
}

void FontCollector::FinalizeUsageCodepoints(UsageData& data) {
	MergePendingCodepoints(data);

	size_t latin1_count = 0;
	for (auto bits : data.latin1_codepoints) {
		while (bits) {
			latin1_count += bits & 1;
			bits >>= 1;
		}
	}

	if (!latin1_count)
		return;

	std::vector<uint32_t> merged;
	merged.reserve(latin1_count + data.codepoints.size());
	for (size_t word = 0; word < data.latin1_codepoints.size(); ++word) {
		auto bits = data.latin1_codepoints[word];
		for (size_t bit = 0; bits && bit < 64; ++bit) {
			if (bits & (uint64_t(1) << bit))
				merged.push_back(static_cast<uint32_t>(word * 64 + bit));
		}
	}

	merged.insert(merged.end(), data.codepoints.begin(), data.codepoints.end());
	data.codepoints.swap(merged);
	data.latin1_codepoints = {};
}

void FontCollector::ResolveFontUsage(StyleInfo const& style, UsageData& data, FontCollectorDetails *details,
                                 std::vector<MissingGlyphQuery>& missing_queries) {
	if (data.codepoints.empty()) return;

	auto const requested_weight = style.bold == 0 ? 400 :
	                              style.bold == 1 ? 700 :
	                                               style.bold;

	auto res = lister->GetFontPaths(style.facename, style.bold, style.italic, data.codepoints);
	for (auto& path : res.paths)
		path.make_preferred();

	if (details) {
		auto& usage = details->fonts.emplace_back();
		usage.ass_facename = style.facename;
		usage.ass_bold = style.bold;
		usage.ass_italic = style.italic;
		usage.codepoints = data.codepoints;
		usage.styles = data.styles;
		usage.override_lines = data.override_lines;
		usage.matched.facename = res.matched_facename;
		usage.matched.face_index = res.face_index;
		usage.matched.weight = res.matched_weight;
		usage.matched.bold = res.matched_bold;
		usage.matched.italic = res.matched_italic;
		usage.matched.is_collection = res.is_collection;
		usage.matched.paths = res.paths;
		usage.matched.path_source = res.path_source;
		usage.matched.raw_data = res.raw_data;
		usage.matched.fake_bold = res.fake_bold;
		usage.matched.fake_italic = res.fake_italic;
		usage.matched.missing_text = res.missing;
		usage.matched.missing_codepoints = DecodeUniqueUtf8Codepoints(res.missing);
		usage.matched.requested_weight = res.requested_weight;

		if (enable_libass_compat_) {
			auto request = NormalizeAssFontRequest(style.facename, style.bold, style.italic);
			FontMatchFaceAttributes face_attributes;
			face_attributes.weight = res.matched_weight ? res.matched_weight : request.requested_weight;
			face_attributes.bold = res.matched_bold;
			face_attributes.italic = res.matched_italic;
			auto synthetic = DetectSyntheticStyle(face_attributes, request);
			usage.matched.libass_fake_bold = synthetic.fake_bold;
			usage.matched.libass_fake_italic = synthetic.fake_italic;
			usage.matched.libass_score = FontAttributesSimilarity(face_attributes, request);
		}
	}

	auto make_event = [&](FontCollectorEventType type) {
		FontCollectorEvent event;
		event.type = type;
		event.face = style.facename;
		event.requested_weight = requested_weight;
		event.requested_italic = style.italic ? 1 : 0;
		return event;
	};

	if (res.paths.empty() && res.raw_data.bytes.empty()) {
		Emit(event_sink, make_event(FontCollectorEventType::FontMissing));
		PrintUsage(data);
		++missing;
	}
	else {
		for (auto& elem : res.paths) {
			if (std::find(begin(results), end(results), elem) == end(results)) {
				auto event = make_event(FontCollectorEventType::FontFound);
				event.path = elem;
				event.message = res.path_source;
				Emit(event_sink, std::move(event));
				results.push_back(elem);
			}
		}

		if (res.paths.empty() && !res.raw_data.bytes.empty()) {
			auto event = make_event(FontCollectorEventType::FontFound);
			event.message = "memory";
			Emit(event_sink, std::move(event));
		}

		if (res.fake_bold)
			Emit(event_sink, make_event(FontCollectorEventType::FakeBold));
		if (res.fake_italic)
			Emit(event_sink, make_event(FontCollectorEventType::FakeItalic));

		if (res.missing.size()) {
			MissingGlyphQuery query;
			query.style = style;
			query.missing_codepoints = DecodeUniqueUtf8Codepoints(res.missing);
			query.event = make_event(FontCollectorEventType::MissingGlyphs);
			query.event.message = res.missing;
			query.event.count = static_cast<int>(query.missing_codepoints.size());
			query.usage = &data;
			missing_queries.push_back(std::move(query));
			++missing_glyphs;
		}
		else if (res.fake_bold || res.fake_italic)
			PrintUsage(data);
	}
}

void FontCollector::CollectMissingGlyphLines(AssFile const *file, int wrap_style, std::vector<MissingGlyphQuery>& queries) {
	if (queries.empty())
		return;

	std::map<StyleInfo, size_t> query_indices;
	for (size_t i = 0; i < queries.size(); ++i) {
		if (!queries[i].missing_codepoints.empty())
			query_indices.emplace(queries[i].style, i);
	}

	if (query_indices.empty())
		return;

	int index = 0;
	for (auto const& diag : file->Events) {
		++index;
		if (diag.Comment)
			continue;

		ForEachLineTextSpan(diag, index, wrap_style, [&](StyleInfo const& style, bool, std::string_view text) {
			auto query_it = query_indices.find(style);
			if (query_it == end(query_indices))
				return false;

			auto& query = queries[query_it->second];
			if (TextContainsAnyCodepoint(text, wrap_style, query.missing_codepoints) &&
			    (query.matching_lines.empty() || query.matching_lines.back() != index))
				query.matching_lines.push_back(index);
			return false;
		}, false);
	}
}

void FontCollector::ProcessDialogueLine(const AssDialogue *line, int index, int wrap_style) {
	if (line->Comment) return;

	ForEachLineTextSpan(*line, index, wrap_style, [&](StyleInfo const& style, bool overriden, std::string_view text) {
		auto& usage = used_styles[style];
		if (overriden) {
			auto& lines = usage.override_lines;
			if (lines.empty() || lines.back() != index)
				lines.push_back(index);
		}

		AppendTextCodepoints(text, wrap_style, usage);
		return false;
	}, true);
}

void FontCollector::PrintUsage(UsageData const& data) {
	PrintUsage(data, data.override_lines);
}

void FontCollector::PrintUsage(UsageData const& data, std::vector<int> const& lines) {
	FontCollectorEvent event;
	event.type = FontCollectorEventType::Usage;
	event.styles = data.styles;
	event.lines = lines;
	Emit(event_sink, std::move(event));
}

std::vector<agi::fs::path> FontCollector::GetFontPaths(const AssFile *file, FontCollectorDetails *details) {
	missing = 0;
	missing_glyphs = 0;
	ass_file = file;
	used_styles.clear();
	missing_style_lines.clear();
	results.clear();
	if (details)
		details->fonts.clear();

	FontCollectorEvent event;
	event.type = FontCollectorEventType::ParsingFile;
	Emit(event_sink, std::move(event));

	for (auto const& style : file->Styles) {
		StyleInfo info = MakeStyleInfo(style);
		used_styles[info].styles.push_back(style.name);
	}

	int wrap_style = file->GetScriptInfoAsInt("WrapStyle");
	int index = 0;
	for (auto const& diag : file->Events)
		ProcessDialogueLine(&diag, ++index, wrap_style);
	EmitMissingStyles();
	for (auto& style : used_styles)
		FinalizeUsageCodepoints(style.second);
	if (details)
		details->fonts.reserve(used_styles.size());

	event = FontCollectorEvent();
	event.type = FontCollectorEventType::SearchingForFontFiles;
	Emit(event_sink, std::move(event));
	std::vector<MissingGlyphQuery> missing_queries;
	for (auto& style : used_styles) ResolveFontUsage(style.first, style.second, details, missing_queries);
	CollectMissingGlyphLines(file, wrap_style, missing_queries);
	for (auto const& query : missing_queries) {
		if (!query.usage)
			continue;
		Emit(event_sink, query.event);
		PrintUsage(*query.usage, query.matching_lines);
	}
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
