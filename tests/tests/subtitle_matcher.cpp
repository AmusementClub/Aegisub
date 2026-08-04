#include "../../src/subtitle_matcher.h"

#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"

#include <string>

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
	s.match_case = false;
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
	return line;
}

// Run a matcher against a line and report whether it matched.
bool matched(const std::function<MatchState(const AssDialogue *, size_t)> &m,
             const AssDialogue &line) {
	return static_cast<bool>(m(&line, 0));
}
}

// ----------------------------------------------------------------------------
// Case-insensitive (default) path: normalization is applied to both sides, so
// precomposed and decomposed forms of the same text must match regardless of
// ASCII case.
// ----------------------------------------------------------------------------
TEST(subtitle_matcher, icase_matches_across_unicode_forms) {
	auto s = base_settings("CAF\xC3\x89"); // "CAFÉ" precomposed, upper
	auto matcher = MakeSubtitleMatcher(s);

	EXPECT_TRUE(matched(matcher, make_line(precomposed_e)));
	EXPECT_TRUE(matched(matcher, make_line(decomposed_e)));
}

// ----------------------------------------------------------------------------
// Regression: match_case must compare raw bytes. Precomposed and decomposed
// forms have different byte sequences, so they must NOT match when the needle
// is in the other form, even though they render identically.
//
// Before the fix, the needle was always NFC-normalized, which silently folded
// these forms together and broke the "match case" contract.
// ----------------------------------------------------------------------------
TEST(subtitle_matcher, match_case_is_byte_exact_for_unicode_forms) {
	// Needle precomposed, haystack decomposed -> different bytes -> no match.
	auto s_pre = base_settings(precomposed_e);
	s_pre.match_case = true;
	EXPECT_FALSE(matched(MakeSubtitleMatcher(s_pre), make_line(decomposed_e)));

	// Needle decomposed, haystack precomposed -> different bytes -> no match.
	auto s_de = base_settings(decomposed_e);
	s_de.match_case = true;
	EXPECT_FALSE(matched(MakeSubtitleMatcher(s_de), make_line(precomposed_e)));

	// Same form on both sides still matches (sanity check).
	EXPECT_TRUE(matched(MakeSubtitleMatcher(s_pre), make_line(precomposed_e)));
	EXPECT_TRUE(matched(MakeSubtitleMatcher(s_de), make_line(decomposed_e)));
}

// ----------------------------------------------------------------------------
// match_case must be ASCII-case sensitive: differing ASCII case must not match.
// ----------------------------------------------------------------------------
TEST(subtitle_matcher, match_case_is_ascii_case_sensitive) {
	auto s = base_settings("Needle");
	s.match_case = true;
	EXPECT_TRUE(matched(MakeSubtitleMatcher(s), make_line("Find Needle here")));
	EXPECT_FALSE(matched(MakeSubtitleMatcher(s), make_line("Find needle here")));
	EXPECT_FALSE(matched(MakeSubtitleMatcher(s), make_line("Find NEEDLE here")));
}

// ----------------------------------------------------------------------------
// Case-insensitive path folds ASCII case.
// ----------------------------------------------------------------------------
TEST(subtitle_matcher, icase_folds_ascii_case) {
	auto s = base_settings("Needle");
	EXPECT_TRUE(matched(MakeSubtitleMatcher(s), make_line("Find needle here")));
	EXPECT_TRUE(matched(MakeSubtitleMatcher(s), make_line("Find NEEDLE here")));
	EXPECT_FALSE(matched(MakeSubtitleMatcher(s), make_line("No match here")));
}

// ----------------------------------------------------------------------------
// Match offsets point into the (possibly normalized) haystack consistently.
// ----------------------------------------------------------------------------
TEST(subtitle_matcher, reports_match_offsets) {
	auto s = base_settings("world");
	auto matcher = MakeSubtitleMatcher(s);
	AssDialogue line = make_line("hello world!");

	auto ms = matcher(&line, 0);
	ASSERT_TRUE(ms);
	EXPECT_EQ(6u, ms.start);
	EXPECT_EQ(11u, ms.end);
}

TEST(subtitle_matcher, regex_replacement_scope_distinguishes_replace_next_and_all) {
	auto s = base_settings("a");
	s.use_regex = true;
	s.replace_with = "$'";
	auto matcher = MakeSubtitleMatcher(s);
	AssDialogue line = make_line("abc");

	auto ms = matcher(&line, 0);
	ASSERT_TRUE(ms);
	EXPECT_EQ("", ExpandSubtitleMatchReplacement(
		ms, s, SubtitleMatchReplacementScope::MATCH_ONLY));
	EXPECT_EQ("bc", ExpandSubtitleMatchReplacement(
		ms, s, SubtitleMatchReplacementScope::SEARCH_CONTEXT));
}

// ----------------------------------------------------------------------------
// An empty replace_with short-circuits the per-match regex format expansion
// (the Find-report recompute path never reads replacements). The match is still
// reported and marked, but both replacement fields stay empty — pinning this
// so a future change to the short-circuit does not silently regress the
// per-keystroke cost.
// ----------------------------------------------------------------------------
TEST(subtitle_matcher, empty_replace_with_short_circuits_format_expansion) {
	auto s = base_settings("\\w+");
	s.use_regex = true;
	// replace_with is "" (base_settings default) — the short-circuit path.
	auto enumerate = MakeSubtitleMatchEnumerator(s);
	AssDialogue line = make_line("abc def");

	auto matches = enumerate(line);
	ASSERT_EQ(2u, matches.size());
	for (auto const& ms : matches) {
		EXPECT_TRUE(ms.has_regex_replacement);
		EXPECT_EQ("", ms.match_only_replacement);
		EXPECT_EQ("", ms.search_context_replacement);
		// ExpandSubtitleMatchReplacement must agree (returns empty for both scopes).
		EXPECT_EQ("", ExpandSubtitleMatchReplacement(
			ms, s, SubtitleMatchReplacementScope::MATCH_ONLY));
		EXPECT_EQ("", ExpandSubtitleMatchReplacement(
			ms, s, SubtitleMatchReplacementScope::SEARCH_CONTEXT));
	}
}
