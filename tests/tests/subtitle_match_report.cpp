#include "../../src/subtitle_match_report.h"

#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"

#include <memory>
#include <string>
#include <vector>

namespace {

// "é" as a single precomposed code point (U+00E9): C3 A9
const std::string precomposed_e = "caf\xC3\xA9";
// "é" as a base letter + combining acute (U+0065 U+0301): 65 CC 81
const std::string decomposed_e = "caf\x65\xCC\x81";

SearchReplaceSettings base_settings(std::string find) {
	SearchReplaceSettings s;
	s.find = std::move(find);
	s.replace_with = "";
	s.field = SearchReplaceSettings::Field::TEXT;
	s.limit_to = SearchReplaceSettings::Limit::ALL;
	s.match_case = true;
	s.use_regex = false;
	s.use_unicode_escapes = false;
	s.ignore_comments = false;
	s.skip_tags = false;
	s.exact_match = false;
	return s;
}

AssDialogue make_line(std::string text) {
	AssDialogue line;
	line.Style = "Default";
	line.Text = std::move(text);
	line.Row = 0;
	return line;
}

struct report_fixture {
	EntryList<AssDialogue> events;
	std::vector<std::unique_ptr<AssDialogue>> storage;

	AssDialogue *AddLine(std::string text, int row = -1) {
		storage.push_back(std::make_unique<AssDialogue>());
		auto *line = storage.back().get();
		line->Style = "Default";
		line->Text = std::move(text);
		line->Row = row >= 0 ? row : static_cast<int>(storage.size() - 1);
		events.push_back(*line);
		return line;
	}
};

}

// ----------------------------------------------------------------------------
// Line eligibility: comment and style filters must match the engine's old
// inline checks so Find All and Replace All cannot disagree.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, line_is_eligible_respects_comment_and_style_filters) {
	auto s = base_settings("x");
	AssDialogue line = make_line("x");

	EXPECT_TRUE(aegisub::subtitle_match_report::LineIsEligible(line, s));

	line.Comment = true;
	s.ignore_comments = true;
	EXPECT_FALSE(aegisub::subtitle_match_report::LineIsEligible(line, s));

	line.Comment = false;
	s.match_styles = { "Alt" };
	EXPECT_FALSE(aegisub::subtitle_match_report::LineIsEligible(line, s));

	line.Style = "Alt";
	EXPECT_TRUE(aegisub::subtitle_match_report::LineIsEligible(line, s));
}

// ----------------------------------------------------------------------------
// Multiple hits on one line; offsets point into the original field text.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, find_in_line_reports_every_match_with_original_offsets) {
	auto s = base_settings("a");
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("banana");

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);

	ASSERT_EQ(3u, hits.size());
	EXPECT_EQ(1u, hits[0].start);
	EXPECT_EQ(2u, hits[0].end);
	EXPECT_EQ(3u, hits[1].start);
	EXPECT_EQ(4u, hits[1].end);
	EXPECT_EQ(5u, hits[2].start);
	EXPECT_EQ(6u, hits[2].end);
	EXPECT_EQ("a", hits[0].matched);
	EXPECT_EQ("banana", *hits[0].line_text);
	EXPECT_EQ(hits[0].line_text, hits[1].line_text);
	EXPECT_EQ(hits[1].line_text, hits[2].line_text);
	EXPECT_EQ(line.Id, hits[0].line_id);
	EXPECT_EQ("Default", hits[0].style);
	EXPECT_FALSE(hits[0].start_time.empty());
	EXPECT_EQ(SearchReplaceSettings::Field::TEXT, hits[0].field);
}

// ----------------------------------------------------------------------------
// Field is recorded so UI reconcile/jump can target Style/Actor/Effect without
// confusing them with Text.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, records_searched_field) {
	auto s = base_settings("Default");
	s.field = SearchReplaceSettings::Field::STYLE;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("body text that also says Default");
	line.Style = "Default";

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ(SearchReplaceSettings::Field::STYLE, hits[0].field);
	EXPECT_EQ("Default", *hits[0].line_text);
	EXPECT_EQ("Default", hits[0].matched);
}

// ----------------------------------------------------------------------------
// Zero-length regex matches must terminate. Patterns like a*, ^, and \b would
// otherwise spin forever under a naive per-match loop.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, empty_regex_matches_terminate) {
	struct Case {
		const char *pattern;
		const char *text;
		// At least one hit, and the loop must return.
		std::size_t min_hits;
	};
	Case cases[] = {
		{ "a*", "bbb", 1 },
		{ "^", "hello", 1 },
		{ "\\b", "hi there", 1 },
	};

	for (auto const& c : cases) {
		auto s = base_settings(c.pattern);
		s.use_regex = true;
		auto matcher = MakeSubtitleMatchEnumerator(s);
		AssDialogue line = make_line(c.text);

		std::vector<aegisub::subtitle_match_report::MatchHit> hits;
		aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
		EXPECT_GE(hits.size(), c.min_hits) << c.pattern;
		// Zero-width patterns can match at every character boundary including
		// the end; an upper bound turns a non-terminating loop into a failure
		// instead of a CI hang.
		EXPECT_LE(hits.size(), std::string(c.text).size() + 1) << c.pattern;
	}
}

TEST(subtitle_match_report, zero_width_match_retries_nonempty_at_same_position) {
	auto s = base_settings("^|a");
	s.use_regex = true;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("a");

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(2u, hits.size());
	EXPECT_EQ(0u, hits[0].start);
	EXPECT_EQ(0u, hits[0].end);
	EXPECT_EQ(0u, hits[1].start);
	EXPECT_EQ(1u, hits[1].end);

	s.replace_with = "X";
	auto replacer = MakeSubtitleMatchEnumerator(s);
	AssDialogue replaced = make_line("a");
	std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements;
	aegisub::subtitle_match_report::ReplaceInLine(replaced, s, replacer, replacements);
	ASSERT_EQ(2u, replacements.size());
	EXPECT_EQ("XX", replaced.Text.get());
}

// ----------------------------------------------------------------------------
// AdvancePastEmptyMatch steps whole UTF-8 characters, not raw bytes.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, advance_past_empty_match_skips_utf8_character) {
	// Two-byte "é" starting at index 3 of "caf\xC3\xA9"
	std::size_t next = aegisub::subtitle_match_report::AdvancePastEmptyMatch(precomposed_e, 3);
	EXPECT_EQ(5u, next);

	// Past end returns size+1 so a caller loop can stop.
	EXPECT_EQ(precomposed_e.size() + 1,
	          aegisub::subtitle_match_report::AdvancePastEmptyMatch(precomposed_e, precomposed_e.size()));
}

// ----------------------------------------------------------------------------
// ReplaceInLine delta: original coordinates stay correct when the replacement
// is longer or shorter than the match.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, replace_in_line_preserves_original_offsets_when_length_changes) {
	// Longer replacement: "a" -> "XX"
	{
		auto s = base_settings("a");
		s.replace_with = "XX";
		auto matcher = MakeSubtitleMatchEnumerator(s);
		AssDialogue line = make_line("a-a");

		std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
		auto count = aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

		ASSERT_EQ(2u, count);
		ASSERT_EQ(2u, hits.size());
		EXPECT_EQ(hits[0].line_text, hits[1].line_text);
		EXPECT_EQ(0u, hits[0].start);
		EXPECT_EQ(1u, hits[0].end);
		EXPECT_EQ(0u, hits[0].new_start);
		EXPECT_EQ(2u, hits[0].new_end);
		EXPECT_EQ(2u, hits[1].start);
		EXPECT_EQ(3u, hits[1].end);
		// After first insert of +1 byte, second match appears at current 3..4
		// in "XX-a", mapping back to original 2..3.
		EXPECT_EQ(3u, hits[1].new_start);
		EXPECT_EQ(5u, hits[1].new_end);
		EXPECT_EQ("XX-XX", line.Text.get());
	}

	// Shorter replacement: "aa" -> "b"
	{
		auto s = base_settings("aa");
		s.replace_with = "b";
		auto matcher = MakeSubtitleMatchEnumerator(s);
		AssDialogue line = make_line("aaXaa");

		std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
		aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

		ASSERT_EQ(2u, hits.size());
		EXPECT_EQ(0u, hits[0].start);
		EXPECT_EQ(2u, hits[0].end);
		EXPECT_EQ(3u, hits[1].start);
		EXPECT_EQ(5u, hits[1].end);
		EXPECT_EQ("bXb", line.Text.get());
	}
}

// ----------------------------------------------------------------------------
// ReplacedLineText: per-hit isolated view of the line after applying only this
// hit's replacement (matches the Replacement column, not the combined result).
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, replaced_line_text_splices_replacement_into_original) {
	auto s = base_settings("a");
	s.replace_with = "XX";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("a-a");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(2u, hits.size());
	// Each hit reflects an isolated single replacement, not the combined "XX-XX".
	EXPECT_EQ("XX-a", aegisub::subtitle_match_report::ReplacedLineText(hits[0]));
	EXPECT_EQ("a-XX", aegisub::subtitle_match_report::ReplacedLineText(hits[1]));
}

TEST(subtitle_match_report, replaced_line_text_handles_shorter_replacement) {
	auto s = base_settings("aa");
	s.replace_with = "b";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("aaXaa");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(2u, hits.size());
	EXPECT_EQ("bXaa", aegisub::subtitle_match_report::ReplacedLineText(hits[0]));
	EXPECT_EQ("aaXb", aegisub::subtitle_match_report::ReplacedLineText(hits[1]));
}

TEST(subtitle_match_report, replaced_line_text_preserves_utf8_boundaries) {
	auto s = base_settings(decomposed_e);
	s.match_case = false;
	s.replace_with = "ZZ";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("xx" + decomposed_e + "yy");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("xxZZyy", aegisub::subtitle_match_report::ReplacedLineText(hits[0]));
}

// ----------------------------------------------------------------------------
// skip_tags: match offsets map back into the original (tagged) field text.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, skip_tags_maps_offsets_to_original_text) {
	auto s = base_settings("world");
	s.skip_tags = true;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	// "hello " + {\b1} + "world"
	AssDialogue line = make_line("hello {\\b1}world");

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("world", hits[0].matched);
	// "hello {\b1}" is 11 bytes; "world" follows.
	EXPECT_EQ(11u, hits[0].start);
	EXPECT_EQ(16u, hits[0].end);
	EXPECT_EQ(line.Text.get(), *hits[0].line_text);
}

// ----------------------------------------------------------------------------
// FindAll walks the event list and honors selection / eligibility filters.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, find_all_respects_selection_and_filters) {
	report_fixture fx;
	auto *a = fx.AddLine("one apple", 0);
	auto *b = fx.AddLine("two apples", 1);
	b->Comment = true;
	auto *c = fx.AddLine("three", 2);

	auto s = base_settings("apple");
	s.ignore_comments = true;

	auto all = aegisub::subtitle_match_report::FindAll(fx.events, s, {});
	ASSERT_EQ(1u, all.size());
	EXPECT_EQ(a->Id, all[0].line_id);

	s.ignore_comments = false;
	s.limit_to = SearchReplaceSettings::Limit::SELECTED;
	auto selected = aegisub::subtitle_match_report::FindAll(fx.events, s, { c, b });
	// Only b has "apple" among {b,c}.
	ASSERT_EQ(1u, selected.size());
	EXPECT_EQ(b->Id, selected[0].line_id);
	EXPECT_EQ("apple", selected[0].matched);
}

// ----------------------------------------------------------------------------
// Zero-width ^ on an immutable original: one hit at the start only (cursor
// advances past the empty match), not a re-match after mutation.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, regex_replace_is_per_match_not_whole_field) {
	auto s = base_settings("^");
	s.use_regex = true;
	s.replace_with = "X";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("ab");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("Xab", line.Text.get());
	EXPECT_EQ(0u, hits[0].start);
	EXPECT_EQ(0u, hits[0].end);
	EXPECT_EQ(0u, hits[0].new_start);
	EXPECT_EQ(1u, hits[0].new_end);
}

// Lookaround sees the immutable original for every hit. Mutate-then-rematch
// would turn (?<=a)a on "aaa" into a single replacement after the first edit.
TEST(subtitle_match_report, regex_replace_all_lookaround_uses_original_context) {
	auto s = base_settings("(?<=a)a");
	s.use_regex = true;
	s.replace_with = "b";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("aaa");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	// Original: positions 1 and 2 are "a" with lookbehind "a".
	ASSERT_EQ(2u, hits.size());
	EXPECT_EQ("abb", line.Text.get());
}

// Boost ${n} form must expand (not remain literal).
TEST(subtitle_match_report, regex_replace_supports_braced_backref) {
	auto s = base_settings("(ab)");
	s.use_regex = true;
	s.replace_with = "<${1}>";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("ab");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("<ab>", line.Text.get());
}

// ----------------------------------------------------------------------------
// Case-insensitive path NFC-normalizes the haystack; reported ranges must
// map back to original UTF-8 so substr / replace never split a code unit.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, icase_nfc_offsets_map_to_original) {
	auto s = base_settings(precomposed_e);
	s.match_case = false;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("xx" + decomposed_e + "yy");

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ(decomposed_e, line.Text.get().substr(hits[0].start, hits[0].end - hits[0].start));

	// Replace must rewrite the original (decomposed) span, not mid-sequence.
	s.replace_with = "ZZ";
	auto replacer = MakeSubtitleMatchEnumerator(s);
	AssDialogue line2 = make_line("xx" + decomposed_e + "yy");
	std::vector<aegisub::subtitle_match_report::ReplacementHit> rhits;
	aegisub::subtitle_match_report::ReplaceInLine(line2, s, replacer, rhits);
	ASSERT_EQ(1u, rhits.size());
	EXPECT_EQ("xxZZyy", line2.Text.get());
}

// ----------------------------------------------------------------------------
// Zero-width regex with a non-empty replacement must terminate (e.g. a* -> X).
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, zero_width_regex_replace_with_nonempty_terminates) {
	auto s = base_settings("a*");
	s.use_regex = true;
	s.replace_with = "X";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("bbb");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	// One insertion per character boundary including end: XbXbXbX
	EXPECT_LE(hits.size(), 4u);
	EXPECT_FALSE(line.Text.get().empty());
	// Must not grow without bound; length is finite and small.
	EXPECT_LE(line.Text.get().size(), 7u);
	EXPECT_EQ("XbXbXbX", line.Text.get());
}

// ----------------------------------------------------------------------------
// Lookbehind must still expand against full-field context. Re-running the
// pattern on the matched substring alone would fail and leave text unchanged
// while still counting a hit.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, regex_lookbehind_expand_uses_full_field) {
	auto s = base_settings("(?<=a)b");
	s.use_regex = true;
	s.replace_with = "Z";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("ab");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("aZ", line.Text.get());
	EXPECT_EQ("b", hits[0].matched);
	EXPECT_EQ("Z", hits[0].replacement);
}

// ----------------------------------------------------------------------------
// NFC reorders combining marks by class (cedilla 202 before acute 230). A map
// that only uses normalized prefix lengths would attribute the wrong original
// mark to a hit on the reordered sequence.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, icase_nfc_combining_reorder_maps_correct_mark) {
	// q + acute (U+0301) + cedilla (U+0327) -> NFC q + cedilla + acute
	const std::string original = std::string("q") + "\xCC\x81" + "\xCC\xA7";
	const std::string cedilla = "\xCC\xA7";
	const std::string acute = "\xCC\x81";

	auto s = base_settings(cedilla);
	s.match_case = false;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line(original);

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ(cedilla, line.Text.get().substr(hits[0].start, hits[0].end - hits[0].start));
	EXPECT_NE(acute, line.Text.get().substr(hits[0].start, hits[0].end - hits[0].start));

	s.replace_with = "X";
	auto replacer = MakeSubtitleMatchEnumerator(s);
	AssDialogue line2 = make_line(original);
	std::vector<aegisub::subtitle_match_report::ReplacementHit> rhits;
	aegisub::subtitle_match_report::ReplaceInLine(line2, s, replacer, rhits);
	ASSERT_EQ(1u, rhits.size());
	// Original order is q+acute+cedilla; replace only the cedilla span.
	EXPECT_EQ(std::string("q") + acute + "X", line2.Text.get());
}

// After matching the original-later mark (acute), the next search must not
// rediscover it just because NFC places acute after cedilla.
TEST(subtitle_match_report, icase_nfc_reorder_does_not_rescan_earlier_original_mark) {
	const std::string original = std::string("q") + "\xCC\x81" + "\xCC\xA7";
	const std::string acute = "\xCC\x81";

	auto s = base_settings(acute);
	s.match_case = false;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line(original);

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	// Exactly one acute in the original — no duplicate from NFC rescan.
	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ(acute, line.Text.get().substr(hits[0].start, hits[0].end - hits[0].start));
}

TEST(subtitle_match_report, icase_regex_enumerates_reordered_codepoints_once) {
	const std::string acute = "\xCC\x81";
	const std::string cedilla = "\xCC\xA7";
	const std::string original = std::string("q") + acute + cedilla;

	auto s = base_settings(".");
	s.match_case = false;
	s.use_regex = true;
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line(original);

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(3u, hits.size());
	EXPECT_EQ("q", hits[0].matched);
	EXPECT_EQ(acute, hits[1].matched);
	EXPECT_EQ(cedilla, hits[2].matched);
	EXPECT_EQ(0u, hits[0].start);
	EXPECT_EQ(1u, hits[1].start);
	EXPECT_EQ(3u, hits[2].start);

	s.replace_with = "X";
	auto replacer = MakeSubtitleMatchEnumerator(s);
	AssDialogue replaced = make_line(original);
	std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements;
	aegisub::subtitle_match_report::ReplaceInLine(replaced, s, replacer, replacements);
	ASSERT_EQ(3u, replacements.size());
	EXPECT_EQ("XXX", replaced.Text.get());
}

// Skip-tags: expand $1 from the tagless match context, not the tagged field.
TEST(subtitle_match_report, skip_tags_regex_backref_uses_match_captures) {
	auto s = base_settings("(ab)");
	s.use_regex = true;
	s.skip_tags = true;
	s.replace_with = "<$1>";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("a{\\b1}b");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("<ab>", line.Text.get());
}

// Regex captures expand on the NFC form used for matching.
TEST(subtitle_match_report, icase_regex_backref_expands_on_nfc_form) {
	auto s = base_settings("(caf\xC3\xA9)");
	s.match_case = false;
	s.use_regex = true;
	s.replace_with = "<$1>";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line(decomposed_e);

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	// Capture is the NFC form of the match (precomposed é).
	EXPECT_EQ("<caf\xC3\xA9>", line.Text.get());
}

TEST(subtitle_match_report, regex_capture_before_reset_start_is_preserved) {
	auto s = base_settings("(a)\\Kb");
	s.use_regex = true;
	s.replace_with = "$1";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("ab");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("aa", line.Text.get());
	EXPECT_EQ("b", hits[0].matched);
	EXPECT_EQ("a", hits[0].replacement);
}

TEST(subtitle_match_report, empty_field_regex_capture_expands) {
	auto s = base_settings("^()$");
	s.use_regex = true;
	s.replace_with = "<$1>";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("<>", line.Text.get());
}

TEST(subtitle_match_report, unicode_case_transform_uses_icu_traits) {
	auto s = base_settings("(\xC3\xA9)");
	s.use_regex = true;
	s.replace_with = "\\U$1\\E";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("\xC3\xA9");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("\xC3\x89", line.Text.get());
}

TEST(subtitle_match_report, regex_prefix_format_uses_search_context) {
	auto s = base_settings("b");
	s.use_regex = true;
	s.replace_with = "$`";
	auto matcher = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("abc");

	std::vector<aegisub::subtitle_match_report::ReplacementHit> hits;
	aegisub::subtitle_match_report::ReplaceInLine(line, s, matcher, hits);

	ASSERT_EQ(1u, hits.size());
	EXPECT_EQ("aac", line.Text.get());
}

// ----------------------------------------------------------------------------
// RecomputeLineHits re-matches a previously reported line against its live
// text and aligns old hits to fresh matches by appearance order. Refreshed
// hits get updated offsets/matched/line_text; leftover hits (fewer fresh
// matches than old) count as orphaned and are left untouched for the caller
// to grey out.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, recompute_refreshes_hits_at_new_offsets) {
	auto s = base_settings("a");
	auto matcher = MakeSubtitleMatchEnumerator(s);

	// Original line had three 'a's in "banana": offsets 1,3,5.
	AssDialogue line = make_line("banana");
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(3u, hits.size());

	// User edits the line: prefix shifts every match by one byte.
	line.Text = "xbanana";

	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(3u, res.refreshed);
	EXPECT_EQ(0u, res.orphaned);
	// Offsets advanced by the one-byte prefix; matched text unchanged.
	EXPECT_EQ(2u, hits[0].start);
	EXPECT_EQ(3u, hits[0].end);
	EXPECT_EQ(4u, hits[1].start);
	EXPECT_EQ(5u, hits[1].end);
	EXPECT_EQ(6u, hits[2].start);
	EXPECT_EQ(7u, hits[2].end);
	EXPECT_EQ("a", hits[0].matched);
	EXPECT_EQ("xbanana", *hits[0].line_text);
}

TEST(subtitle_match_report, recompute_orphans_hits_when_matches_shrink) {
	auto s = base_settings("a");
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("banana");
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(3u, hits.size());

	// Edit removes two of the three 'a's: only the first survives, at a new
	// offset ("ban" -> the lone 'a' is now at index 1 still, but the later
	// matches are gone).
	line.Text = "banxxx";

	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(1u, res.refreshed);
	EXPECT_EQ(2u, res.orphaned);
	// First hit realigned to the surviving match; offset unchanged here.
	EXPECT_EQ(1u, hits[0].start);
	EXPECT_EQ(2u, hits[0].end);
	EXPECT_EQ("banxxx", *hits[0].line_text);
	// Orphaned hits keep their old matched fragment (no fresh match exists),
	// but line_text/metadata are refreshed so the Context column does not show
	// stale text alongside a surviving sibling row on the same line.
	EXPECT_EQ("banxxx", *hits[1].line_text);
	EXPECT_EQ("banxxx", *hits[2].line_text);
	// Old matched fragment retained (a was at original offsets 3/5).
	EXPECT_EQ("a", hits[1].matched);
	EXPECT_EQ("a", hits[2].matched);
}

TEST(subtitle_match_report, recompute_orphans_all_when_line_no_longer_matches) {
	auto s = base_settings("foo");
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("foo bar foo");
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(2u, hits.size());

	// Match word removed entirely.
	line.Text = "bar baz";

	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(0u, res.refreshed);
	EXPECT_EQ(2u, res.orphaned);
}

TEST(subtitle_match_report, recompute_orphans_all_when_line_becomes_ineligible) {
	auto s = base_settings("a");
	s.ignore_comments = true;
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("banana");
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(3u, hits.size());

	// Turning the line into a comment makes it ineligible under the settings;
	// every hit is orphaned without running the matcher.
	line.Comment = true;

	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(0u, res.refreshed);
	EXPECT_EQ(3u, res.orphaned);
}

TEST(subtitle_match_report, recompute_only_touches_hits_for_the_given_line) {
	auto s = base_settings("a");
	auto matcher = MakeSubtitleMatchEnumerator(s);

	report_fixture fx;
	auto *line1 = fx.AddLine("banana"); // 3 hits (offsets 1,3,5)
	auto *line2 = fx.AddLine("papaya"); // 3 hits (offsets 1,3,5)

	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(*line1, s, matcher, hits);
	aegisub::subtitle_match_report::FindInLine(*line2, s, matcher, hits);
	ASSERT_EQ(6u, hits.size());
	auto const line2_first_start = hits[3].start;
	auto const line2_first_text = hits[3].line_text;

	// Edit only line1; line2's hits must be left untouched.
	line1->Text = "xbanana";
	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		*line1, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(3u, res.refreshed);
	EXPECT_EQ(0u, res.orphaned);
	// line2 hits unchanged (start, line_text, matched all preserved).
	EXPECT_EQ(line2_first_start, hits[3].start);
	EXPECT_EQ(line2_first_text, hits[3].line_text);
}

TEST(subtitle_match_report, recompute_aligns_regex_matches_in_order) {
	auto s = base_settings("\\d+");
	s.use_regex = true;
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("a1b22c333");
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(3u, hits.size());

	// Insert a one-byte prefix; offsets advance by one but order is preserved.
	line.Text = "za1b22c333";
	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	ASSERT_EQ(3u, res.refreshed);
	EXPECT_EQ("1", hits[0].matched);
	EXPECT_EQ("22", hits[1].matched);
	EXPECT_EQ("333", hits[2].matched);
	EXPECT_EQ(2u, hits[0].start);  // z a [1]
	EXPECT_EQ(4u, hits[1].start);  // z a 1 b [2]2
	EXPECT_EQ(7u, hits[2].start);  // z a 1 b 2 2 c [3]33
}

// ----------------------------------------------------------------------------
// When the edited line has MORE matches than before, the extra fresh matches
// are dropped: the panel lists a fixed number of rows and cannot grow new ones
// from a recompute. This pins that behaviour so a future change notices.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, recompute_drops_extra_matches_when_count_grows) {
	auto s = base_settings("a");
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("banana"); // 3 hits
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(3u, hits.size());

	// Editing adds a 4th 'a'. The recompute refreshes the 3 existing rows at
	// their new offsets; the brand-new 4th match is not reported because there
	// is no row to align it to.
	line.Text = "bananaxa"; // a at 1,3,5,7
	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(3u, res.refreshed);
	EXPECT_EQ(0u, res.orphaned);
	EXPECT_EQ(1u, hits[0].start);
	EXPECT_EQ(3u, hits[1].start);
	EXPECT_EQ(5u, hits[2].start);
	EXPECT_EQ("bananaxa", *hits[2].line_text);
}

// ----------------------------------------------------------------------------
// Recompute works on non-TEXT fields and does not rewrite `field`.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, recompute_supports_non_text_field_without_rewriting_it) {
	auto s = base_settings("Default");
	s.field = SearchReplaceSettings::Field::STYLE;
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("body text");
	line.Style = "Default"; // one match in the Style field
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(1u, hits.size());
	ASSERT_EQ(SearchReplaceSettings::Field::STYLE, hits[0].field);

	// Rename to a value that still contains "Default" as a substring: the match
	// survives (substring search), realigned to the new field text.
	line.Style = "DefaultBold";
	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(1u, res.refreshed);
	EXPECT_EQ(0u, res.orphaned);
	// field is preserved, line_text now reflects the renamed style.
	EXPECT_EQ(SearchReplaceSettings::Field::STYLE, hits[0].field);
	EXPECT_EQ("DefaultBold", *hits[0].line_text);

	// Now rename to something the needle is not a substring of: the hit orphans
	// but line_text is still refreshed to the live field text.
	line.Style = "Title";
	res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());
	EXPECT_EQ(0u, res.refreshed);
	EXPECT_EQ(1u, res.orphaned);
	EXPECT_EQ("Title", *hits[0].line_text);
	EXPECT_EQ(SearchReplaceSettings::Field::STYLE, hits[0].field);
}

// ----------------------------------------------------------------------------
// Even when every hit orphans, line_text/metadata are refreshed first, so the
// Context column never lags behind the live text on a fully-greyed line.
// ----------------------------------------------------------------------------
TEST(subtitle_match_report, recompute_refreshes_line_text_even_when_all_orphan) {
	auto s = base_settings("foo");
	auto matcher = MakeSubtitleMatchEnumerator(s);

	AssDialogue line = make_line("foo bar foo");
	std::vector<aegisub::subtitle_match_report::MatchHit> hits;
	aegisub::subtitle_match_report::FindInLine(line, s, matcher, hits);
	ASSERT_EQ(2u, hits.size());

	line.Text = "completely different";
	auto res = aegisub::subtitle_match_report::RecomputeLineHits(
		line, s, matcher, hits.begin(), hits.end());

	EXPECT_EQ(0u, res.refreshed);
	EXPECT_EQ(2u, res.orphaned);
	// No fresh matches, but line_text was still updated to the live text.
	EXPECT_EQ("completely different", *hits[0].line_text);
	EXPECT_EQ("completely different", *hits[1].line_text);
	// Old matched fragment retained (there is nothing accurate to replace it).
	EXPECT_EQ("foo", hits[0].matched);
}
