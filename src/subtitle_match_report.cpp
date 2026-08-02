/// @file subtitle_match_report.cpp
/// @see subtitle_match_report.h

#include "subtitle_match_report.h"

#include "ass_dialogue.h"
#include "ass_file.h"

#include <libaegisub/exception.h>
#include <libaegisub/string_utils.h>

#include <algorithm>

namespace {

auto get_dialogue_field(SearchReplaceSettings::Field field) -> decltype(&AssDialogueBase::Text) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return &AssDialogueBase::Text;
		case SearchReplaceSettings::Field::STYLE: return &AssDialogueBase::Style;
		case SearchReplaceSettings::Field::ACTOR: return &AssDialogueBase::Actor;
		case SearchReplaceSettings::Field::EFFECT: return &AssDialogueBase::Effect;
	}
	throw agi::InternalError("Bad field for search");
}

}



namespace aegisub::subtitle_match_report {

bool LineIsEligible(AssDialogue const& line, SearchReplaceSettings const& settings) {
	if (settings.ignore_comments && line.Comment)
		return false;

	if (settings.match_styles.empty())
		return true;

	for (auto const& style : settings.match_styles) {
		if (line.Style.get() == style)
			return true;
	}
	return false;
}

std::size_t AdvancePastEmptyMatch(std::string const& text, std::size_t pos) {
	if (pos >= text.size())
		return text.size() + 1;

	// Skip the lead byte, then any UTF-8 continuation bytes, so that the next
	// search never starts in the middle of a character.
	std::size_t next = pos + 1;
	while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80)
		++next;
	return next;
}

void FindInLine(AssDialogue const& line, SearchReplaceSettings const& settings,
	             MatchEnumerator& enumerate, std::vector<MatchHit>& out) {
	auto const& text = (line.*get_dialogue_field(settings.field)).get();
	auto matches = enumerate(line);
	if (matches.empty())
		return;

	// NFC can reorder combining marks, so searchable-surface order is not
	// necessarily original byte order. Reports and replacement splicing use
	// original coordinates and therefore need a stable original-order view.
	std::stable_sort(matches.begin(), matches.end(), [](MatchState const& a, MatchState const& b) {
		return a.start < b.start || (a.start == b.start && a.end < b.end);
	});
	auto const line_text = std::make_shared<std::string const>(text);

	for (auto const& ms : matches) {
		MatchHit hit;
		hit.line_id = line.Id;
		hit.row = line.Row;
		hit.start_time = line.Start.GetAssFormatted();
		hit.style = line.Style.get();
		hit.field = settings.field;
		hit.start = ms.start;
		hit.end = ms.end;
		hit.matched = line_text->substr(ms.start, ms.end - ms.start);
		hit.line_text = line_text;
		out.push_back(std::move(hit));
	}
}

std::vector<MatchHit> FindAll(EntryList<AssDialogue> const& events,
                              SearchReplaceSettings const& settings,
                              std::vector<AssDialogue const *> const& selection) {
	std::vector<MatchHit> hits;
	// Build once and reuse: for a regex search this is the only compile.
	MatchEnumerator enumerate = MakeSubtitleMatchEnumerator(settings);

	bool const selection_only = settings.limit_to == SearchReplaceSettings::Limit::SELECTED;
	std::vector<AssDialogue const *> sorted_selection;
	if (selection_only) {
		sorted_selection = selection;
		std::sort(sorted_selection.begin(), sorted_selection.end());
	}

	for (auto const& line : events) {
		if (selection_only &&
		    !std::binary_search(sorted_selection.begin(), sorted_selection.end(), &line))
			continue;
		if (!LineIsEligible(line, settings))
			continue;

		FindInLine(line, settings, enumerate, hits);
	}

	return hits;
}

std::size_t ReplaceInLine(AssDialogue& line, SearchReplaceSettings const& settings,
	                      MatchEnumerator& enumerate, std::vector<ReplacementHit>& out) {
	auto field = get_dialogue_field(settings.field);
	auto matches = enumerate(line);
	if (matches.empty())
		return 0;
	auto const original = std::make_shared<std::string const>((line.*field).get());
	std::stable_sort(matches.begin(), matches.end(), [](MatchState const& a, MatchState const& b) {
		return a.start < b.start || (a.start == b.start && a.end < b.end);
	});

	// Enumerate every hit against the immutable original field so lookaround
	// and other context-sensitive regexes see the pre-edit text for each match
	// (same contract as a whole-field replace-all over non-overlapping finds).
	std::vector<ReplacementHit> pending;
	pending.reserve(matches.size());
	for (auto const& ms : matches) {
		auto const replacement = ExpandSubtitleMatchReplacement(
			ms, settings, SubtitleMatchReplacementScope::SEARCH_CONTEXT);

		ReplacementHit hit;
		hit.line_id = line.Id;
		hit.row = line.Row;
		hit.start_time = line.Start.GetAssFormatted();
		hit.style = line.Style.get();
		hit.field = settings.field;
		hit.start = ms.start;
		hit.end = ms.end;
		hit.matched = original->substr(hit.start, hit.end - hit.start);
		hit.line_text = original;
		hit.replacement = replacement;
		pending.push_back(std::move(hit));
	}

	// Splice replacements into a new string left-to-right; earlier original
	// offsets stay valid because we never mutate before reading the next hit.
	std::string result;
	result.reserve(original->size());
	std::size_t last = 0;
	for (auto& hit : pending) {
		result.append(*original, last, hit.start - last);
		hit.new_start = result.size();
		result.append(hit.replacement);
		hit.new_end = result.size();
		last = hit.end;
	}
	result.append(*original, last, std::string::npos);
	line.*field = std::move(result);

	std::size_t const count = pending.size();
	out.insert(out.end(),
	           std::make_move_iterator(pending.begin()),
	           std::make_move_iterator(pending.end()));
	return count;
}

std::string ReplacedLineText(ReplacementHit const& hit) {
	std::string out;
	if (!hit.line_text)
		return out;
	auto const& original = *hit.line_text;
	// start/end are clamped defensively: callers (ReplaceInLine, tests) produce
	// in-range coordinates, but a malformed hit must never read out of bounds.
	auto const start = hit.start <= original.size() ? hit.start : original.size();
	auto const end = hit.end <= original.size() ? hit.end : original.size();
	out.reserve(start + hit.replacement.size() + (original.size() - end));
	out.append(original, 0, start);
	out.append(hit.replacement);
	out.append(original, end, std::string::npos);
	return out;
}

}
