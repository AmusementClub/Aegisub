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

std::string_view TextBlockView(AssDialogueBlock& block) {
	if (block.GetType() == AssBlockType::PLAIN)
		return static_cast<AssDialogueBlockPlain&>(block).text;
	if (block.GetType() == AssBlockType::DRAWING)
		return static_cast<AssDialogueBlockDrawing&>(block).text;
	return {};
}

template<class Callback>
bool ForEachAssTextCodepoint(std::string_view text, int wrap_style, Callback&& callback) {
	auto const *data = text.data();
	auto const size = text.size();
	for (size_t i = 0; i < size; ) {
		uint32_t value = 0;
		if (text[i] == '\t') {
			// libass treats literal tab characters as ordinary spaces.
			value = 0x20;
			++i;
		}
		else if (text[i] == '\\' && i + 1 < size) {
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
			else if (next == '{' || next == '}') {
				++i;
				value = static_cast<unsigned char>(next);
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

std::string EncodeCodepointsToUtf8(std::vector<uint32_t> const& codepoints) {
	std::string text;
	for (auto codepoint : codepoints) {
		font_collector::unicode::Rune rune;
		if (font_collector::unicode::Rune::TryCreate(codepoint, rune))
			font_collector::unicode::AppendRuneToUtf8(text, rune);
	}
	return text;
}

std::vector<uint32_t> IntersectSortedCodepoints(std::vector<uint32_t> const& left, std::vector<uint32_t> const& right) {
	std::vector<uint32_t> result;
	result.reserve(std::min(left.size(), right.size()));
	std::set_intersection(begin(left), end(left), begin(right), end(right), std::back_inserter(result));
	return result;
}

void MergeSortedCodepointsInto(std::vector<uint32_t>& destination, std::vector<uint32_t> const& source) {
	if (source.empty())
		return;

	if (destination.empty()) {
		destination = source;
		return;
	}

	std::vector<uint32_t> merged;
	merged.reserve(destination.size() + source.size());
	std::set_union(begin(destination), end(destination), begin(source), end(source), std::back_inserter(merged));
	destination.swap(merged);
}

int RequestedWeightForStyle(int bold) {
	return bold == 0 ? 400 :
	       bold == 1 ? 700 :
	                   bold;
}

int SourceLineNumber(AssDialogue const& line, int dialogue_index) {
	return line.Row >= 0 ? line.Row + 1 : dialogue_index;
}

std::unique_ptr<IFontFileLister> CreateDefaultFontFileLister(FontCollectorEventSink& event_sink) {
	return std::make_unique<FontFileLister>(event_sink);
}
}

FontCollector::FontCollector(FontCollectorEventSink event_sink)
: event_sink(std::move(event_sink))
, owned_lister(CreateDefaultFontFileLister(this->event_sink))
, lister(owned_lister.get())
{
}

FontCollector::FontCollector(FontCollectorEventSink event_sink, std::unique_ptr<IFontFileLister> lister)
: event_sink(std::move(event_sink))
, owned_lister(std::move(lister))
, lister(owned_lister.get())
{
}

FontCollector::FontCollector(FontCollectorEventSink event_sink, IFontFileLister& lister)
: event_sink(std::move(event_sink))
, lister(&lister)
{
}

FontCollector::StyleInfo FontCollector::MakeStyleInfo(AssStyle const& style) const {
	StyleInfo info;
	info.facename = style.font;
	info.bold = style.bold;
	info.italic = style.italic;
	return info;
}

void FontCollector::RecordMissingStyle(MissingStyleLines& missing_style_lines, std::string const& name, int line_index) {
	auto& lines = missing_style_lines[name];
	if (line_index > 0 && (lines.empty() || lines.back() != line_index))
		lines.push_back(line_index);

	if (line_index <= 0 && lines.empty())
		lines.push_back(line_index);
}

void FontCollector::EmitMissingStyles(FileAnalysis& analysis) {
	for (auto const& [name, lines] : analysis.missing_style_lines) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::StyleMissing;
		event.style = name;
		event.lines.reserve(lines.size());
		for (int line : lines) {
			if (line > 0)
				event.lines.push_back(line);
		}
		Emit(analysis.event_sink, std::move(event));
		++analysis.missing;
	}
}

template<class Callback>
bool FontCollector::ForEachLineTextSpan(AssFile const& file, AssDialogue const& line, int line_index, int wrap_style,
                                        Callback&& callback, MissingStyleLines *missing_style_lines) {
	auto *initial_style = aegisub::ass_style_resolution::ResolveEventStyle(file, line.Style);
	if (!initial_style) {
		if (missing_style_lines)
			RecordMissingStyle(*missing_style_lines, line.Style, line_index);
		return false;
	}

	StyleInfo style = MakeStyleInfo(*initial_style);
	StyleInfo initial = style;
	bool style_valid = true;

	bool overriden = false;
	int active_wrap_style = wrap_style;
	int active_drawing_level = 0;

	auto process_tags = [&](auto&& self, std::vector<AssOverrideTag> const& tags) -> void {
			for (auto const& tag : tags) {
				if (tag.Name == "\\r") {
					auto const& param = tag.Params[0];
					if (param.omitted || param.empty) {
						style = initial;
						style_valid = true;
						overriden = false;
					}
					else {
						auto style_name = param.Get<std::string>();
						auto *reset_style = aegisub::ass_style_resolution::ResolveResetStyle(file, style_name);
						if (reset_style) {
							style = MakeStyleInfo(*reset_style);
							style_valid = true;
							overriden = false;
						}
						else {
							style_valid = false;
							overriden = false;
							if (missing_style_lines)
								RecordMissingStyle(*missing_style_lines, style_name, line_index);
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
				else if (tag.Name == "\\q") {
					auto value = tag.Params[0].Get(wrap_style);
					active_wrap_style = value >= 0 && value <= 3 ? value : wrap_style;
				}
				else if (tag.Name == "\\p") {
					auto value = tag.Params[0].Get(0);
					active_drawing_level = value > 0 ? value : 0;
				}
				else if (tag.Name == "\\t" && tag.Params.size() > 3 &&
				         !tag.Params[3].omitted && !tag.Params[3].empty) {
					// libass applies font-affecting tags nested in \t, including
					// \fn/\b/\i/\r, even though most transforms are animated.
					if (auto *nested = tag.Params[3].Get<AssDialogueBlockOverride*>())
						self(self, nested->Tags);
				}
			}
	};

	for (auto& block : line.ParseTags()) {
		switch (block->GetType()) {
		case AssBlockType::OVERRIDE:
			process_tags(process_tags, static_cast<AssDialogueBlockOverride&>(*block).Tags);
			break;
		case AssBlockType::PLAIN:
		case AssBlockType::DRAWING: {
			if (active_drawing_level != 0)
				break;
			auto text = TextBlockView(*block);

			if (text.empty() || !style_valid)
				continue;

			if (callback(style, overriden, active_wrap_style, text))
				return true;
			break;
		}
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

FontCollector::ResolvedUsage FontCollector::ResolveMergedUsage(StyleInfo const& style, std::vector<uint32_t> const& codepoints) {
	ResolvedUsage resolved;
	if (codepoints.empty())
		return resolved;

	auto& res = resolved.result;
	res = lister->GetFontPaths(style.facename, style.bold, style.italic, codepoints);
	for (auto& path : res.paths)
		path.make_preferred();

	resolved.missing_codepoints = DecodeUniqueUtf8Codepoints(res.missing);
	return resolved;
}

void FontCollector::ApplyResolvedFontUsage(FileAnalysis& analysis, StyleInfo const& style, UsageData& data,
                                           ResolvedUsage const& resolved, std::vector<MissingGlyphQuery>& missing_queries) {
	if (data.codepoints.empty()) return;

	auto const requested_weight = RequestedWeightForStyle(style.bold);
	auto const& res = resolved.result;
	auto missing_codepoints = IntersectSortedCodepoints(data.codepoints, resolved.missing_codepoints);
	auto missing_text = EncodeCodepointsToUtf8(missing_codepoints);

	if (analysis.details) {
		auto& usage = analysis.details->fonts.emplace_back();
		usage.ass_facename = style.facename;
		usage.ass_bold = style.bold;
		usage.ass_italic = style.italic;
		usage.codepoints = data.codepoints;
		usage.styles = data.styles;
		usage.lines = data.lines;
		usage.override_lines = data.override_lines;
		usage.matched.facename = res.matched_facename;
		usage.matched.facename_full = res.matched_facename_full;
		usage.matched.names = res.matched_names;
		usage.matched.face_index = res.face_index;
		usage.matched.weight = res.matched_weight;
		usage.matched.bold = res.matched_bold;
		usage.matched.italic = res.matched_italic;
		usage.matched.paths = res.paths;
		usage.matched.memory_fonts = res.memory_fonts;
		usage.matched.path_source = res.path_source;
		usage.matched.fake_bold = res.fake_bold;
		usage.matched.fake_italic = res.fake_italic;
		usage.matched.missing_text = missing_text;
		usage.matched.missing_codepoints = missing_codepoints;
		usage.matched.requested_weight = res.requested_weight;
		usage.matched.match_candidates = res.match_candidates;
		usage.matched.match_ambiguous = res.match_ambiguous;

	}

	auto make_event = [&](FontCollectorEventType type) {
		FontCollectorEvent event;
		event.type = type;
		event.face = style.facename;
		event.requested_weight = requested_weight;
		event.requested_italic = style.italic ? 1 : 0;
		return event;
	};

	if (res.paths.empty() && res.memory_fonts.empty()) {
		Emit(analysis.event_sink, make_event(FontCollectorEventType::FontMissing));
		PrintUsage(analysis.event_sink, data);
		++analysis.missing;
	}
	else {
		for (auto& elem : res.paths) {
			if (std::find(begin(analysis.results), end(analysis.results), elem) == end(analysis.results)) {
				auto event = make_event(FontCollectorEventType::FontFound);
				event.path = elem;
				event.message = res.path_source;
				Emit(analysis.event_sink, std::move(event));
				analysis.results.push_back(elem);
			}
		}
		for (auto const& memory_font : res.memory_fonts) {
			if (!memory_font.data || memory_font.data->empty())
				continue;
			auto event = make_event(FontCollectorEventType::FontFound);
			event.message = "memory";
			Emit(analysis.event_sink, std::move(event));
		}

		if (res.fake_bold)
			Emit(analysis.event_sink, make_event(FontCollectorEventType::FakeBold));
		if (res.fake_italic)
			Emit(analysis.event_sink, make_event(FontCollectorEventType::FakeItalic));

		if (!missing_codepoints.empty()) {
			MissingGlyphQuery query;
			query.style = style;
			query.missing_codepoints = missing_codepoints;
			query.event = make_event(FontCollectorEventType::MissingGlyphs);
			query.event.message = missing_text;
			query.event.count = static_cast<int>(query.missing_codepoints.size());
			query.usage = &data;
			missing_queries.push_back(std::move(query));
			++analysis.missing_glyphs;
		}
		else if (res.fake_bold || res.fake_italic)
			PrintUsage(analysis.event_sink, data);
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
		auto const source_line = SourceLineNumber(diag, index);
		if (diag.Comment)
			continue;

		ForEachLineTextSpan(*file, diag, source_line, wrap_style, [&](StyleInfo const& style, bool, int span_wrap_style, std::string_view text) {
			auto query_it = query_indices.find(style);
			if (query_it == end(query_indices))
				return false;

			auto& query = queries[query_it->second];
			if (TextContainsAnyCodepoint(text, span_wrap_style, query.missing_codepoints) &&
			    (query.matching_lines.empty() || query.matching_lines.back() != source_line))
				query.matching_lines.push_back(source_line);
			return false;
		}, nullptr);
	}
}

void FontCollector::StoreMissingGlyphLines(FontCollectorDetails *details, std::vector<MissingGlyphQuery> const& queries) {
	if (!details || queries.empty())
		return;

	for (auto const& query : queries) {
		if (!query.usage)
			continue;

		for (auto& usage : details->fonts) {
			if (usage.ass_facename == query.style.facename &&
			    usage.ass_bold == query.style.bold &&
			    usage.ass_italic == query.style.italic) {
				usage.matched.missing_lines = query.matching_lines;
				break;
			}
		}
	}
}

FontCollector::FileAnalysis FontCollector::AnalyzeFile(FontCollectorBatchSource const& source) {
	FileAnalysis analysis;
	analysis.file = source.file;
	analysis.event_sink = source.event_sink ? source.event_sink : event_sink;
	analysis.details = source.details;
	if (analysis.details)
		analysis.details->fonts.clear();

	FontCollectorEvent event;
	event.type = FontCollectorEventType::ParsingFile;
	Emit(analysis.event_sink, std::move(event));

	if (!analysis.file)
		return analysis;

	for (auto const& style : analysis.file->Styles) {
		StyleInfo info = MakeStyleInfo(style);
		analysis.used_styles[info].styles.push_back(style.name);
	}

	analysis.wrap_style = analysis.file->GetScriptInfoAsInt("WrapStyle");
	int index = 0;
	for (auto const& diag : analysis.file->Events) {
		++index;
		ProcessDialogueLine(analysis, &diag, SourceLineNumber(diag, index));
	}

	EmitMissingStyles(analysis);
	for (auto& style : analysis.used_styles)
		FinalizeUsageCodepoints(style.second);
	if (analysis.details)
		analysis.details->fonts.reserve(analysis.used_styles.size());

	return analysis;
}

void FontCollector::ProcessDialogueLine(FileAnalysis& analysis, const AssDialogue *line, int index) {
	if (line->Comment) return;

	ForEachLineTextSpan(*analysis.file, *line, index, analysis.wrap_style, [&](StyleInfo const& style, bool overriden, int span_wrap_style, std::string_view text) {
		auto& usage = analysis.used_styles[style];
		if (usage.lines.empty() || usage.lines.back() != index)
			usage.lines.push_back(index);
		if (overriden) {
			auto& lines = usage.override_lines;
			if (lines.empty() || lines.back() != index)
				lines.push_back(index);
		}

		AppendTextCodepoints(text, span_wrap_style, usage);
		return false;
	}, &analysis.missing_style_lines);
}

void FontCollector::PrintUsage(FontCollectorEventSink const& sink, UsageData const& data) {
	PrintUsage(sink, data, data.lines);
}

void FontCollector::PrintUsage(FontCollectorEventSink const& sink, UsageData const& data, std::vector<int> const& lines) {
	FontCollectorEvent event;
	event.type = FontCollectorEventType::Usage;
	event.styles = data.styles;
	event.lines = lines;
	Emit(sink, std::move(event));
}

std::vector<agi::fs::path> FontCollector::GetFontPaths(const AssFile *file, FontCollectorDetails *details) {
	FontCollectorBatchSource source;
	source.file = file;
	source.event_sink = event_sink;
	source.details = details;
	auto results = GetFontPaths(std::vector<FontCollectorBatchSource>{source});
	return results.empty() ? std::vector<agi::fs::path>() : std::move(results.front());
}

std::vector<std::vector<agi::fs::path>> FontCollector::GetFontPaths(std::vector<FontCollectorBatchSource> const& sources) {
	std::vector<FileAnalysis> analyses;
	analyses.reserve(sources.size());
	for (auto const& source : sources)
		analyses.push_back(AnalyzeFile(source));

	for (auto& analysis : analyses) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::SearchingForFontFiles;
		Emit(analysis.event_sink, std::move(event));
	}

	std::map<StyleInfo, std::vector<uint32_t>> merged_codepoints;
	for (auto const& analysis : analyses) {
		for (auto const& [style, data] : analysis.used_styles)
			MergeSortedCodepointsInto(merged_codepoints[style], data.codepoints);
	}

	std::map<StyleInfo, ResolvedUsage> resolved;
	for (auto const& [style, codepoints] : merged_codepoints) {
		if (!codepoints.empty())
			resolved.emplace(style, ResolveMergedUsage(style, codepoints));
	}

	std::vector<std::vector<agi::fs::path>> paths_by_file;
	paths_by_file.reserve(analyses.size());
	for (auto& analysis : analyses) {
		std::vector<MissingGlyphQuery> missing_queries;
		for (auto& [style, data] : analysis.used_styles) {
			auto resolved_it = resolved.find(style);
			if (resolved_it != end(resolved))
				ApplyResolvedFontUsage(analysis, style, data, resolved_it->second, missing_queries);
		}

		if (analysis.file)
			CollectMissingGlyphLines(analysis.file, analysis.wrap_style, missing_queries);
		StoreMissingGlyphLines(analysis.details, missing_queries);
		for (auto const& query : missing_queries) {
			if (!query.usage)
				continue;
			Emit(analysis.event_sink, query.event);
			PrintUsage(analysis.event_sink, *query.usage, query.matching_lines);
		}

		FontCollectorEvent event;
		event.type = FontCollectorEventType::SearchComplete;
		Emit(analysis.event_sink, std::move(event));

		event = FontCollectorEvent();
		if (analysis.missing == 0) {
			event.type = FontCollectorEventType::AllFontsFound;
			Emit(analysis.event_sink, std::move(event));
		}
		else {
			event.type = FontCollectorEventType::FontsMissing;
			event.count = analysis.missing;
			Emit(analysis.event_sink, std::move(event));
		}
		if (analysis.missing_glyphs != 0) {
			event = FontCollectorEvent();
			event.type = FontCollectorEventType::FontsMissingGlyphs;
			event.count = analysis.missing_glyphs;
			Emit(analysis.event_sink, std::move(event));
		}

		paths_by_file.push_back(analysis.results);
	}

	return paths_by_file;
}

bool FontCollector::StyleInfo::operator<(StyleInfo const& rgt) const {
	return std::tie(facename, bold, italic) < std::tie(rgt.facename, rgt.bold, rgt.italic);
}
