#include <libaegisub/string_utils.h>

#include <main.h>

#include <array>

namespace {
using agi::util::strings::sized_match;
using agi::util::strings::expand_unicode_codepoint_escapes;
using agi::util::strings::find;
using agi::util::strings::npos;
using agi::util::strings::utf8_find_icase;
using agi::util::strings::utf8_icase_searcher;

void expect_same_match(sized_match const& actual, sized_match const& expected) {
	EXPECT_EQ(expected.offset, actual.offset);
	EXPECT_EQ(expected.length, actual.length);
}
}

TEST(lagi_string_utils, utf8_icase_searcher_matches_direct_search) {
	struct sample {
		char const* haystack;
		char const* needle;
	};

	std::array const samples = {
		sample{"Alpha Beta Needle Omega", "needle"},
		sample{"Alpha Beta Gamma Omega", "needle"},
		sample{"Stra\xC3\x9F""e and more", "STRASSE"},
		sample{"\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C", "\xE4\xB8\x96\xE7\x95\x8C"},
		sample{"Cafe \xE4\xBD\xA0\xE5\xA5\xBD Stra\xC3\x9F""e", "strasse"},
		sample{"", ""},
	};

	for (auto const& sample : samples) {
		auto direct = utf8_find_icase(sample.haystack, sample.needle);
		auto searcher = utf8_icase_searcher(sample.needle);
		auto cached = utf8_find_icase(sample.haystack, searcher);
		expect_same_match(cached, direct);
	}
}

TEST(lagi_string_utils, utf8_icase_searcher_can_be_reused) {
	utf8_icase_searcher searcher("STRASSE");

	expect_same_match(utf8_find_icase("Stra\xC3\x9F""e", searcher), sized_match{0, 7});
	expect_same_match(utf8_find_icase("prefix Stra\xC3\x9F""e suffix", searcher), sized_match{7, 7});
	expect_same_match(utf8_find_icase("plain ascii only", searcher), sized_match{});
}

TEST(lagi_string_utils, expand_unicode_codepoint_escapes_supports_search_forms) {
	std::string expanded;

	ASSERT_TRUE(expand_unicode_codepoint_escapes("\\u4F60\\u597D", expanded));
	EXPECT_EQ("\xE4\xBD\xA0\xE5\xA5\xBD", expanded);

	ASSERT_TRUE(expand_unicode_codepoint_escapes("u+4f60/U+1F600", expanded));
	EXPECT_EQ("\xE4\xBD\xA0/\xF0\x9F\x98\x80", expanded);

	ASSERT_TRUE(expand_unicode_codepoint_escapes("prefix U+0041 suffix", expanded));
	EXPECT_EQ("prefix A suffix", expanded);
}

TEST(lagi_string_utils, expand_unicode_codepoint_escapes_rejects_invalid_input) {
	std::string expanded;

	EXPECT_FALSE(expand_unicode_codepoint_escapes("\\u12", expanded));
	EXPECT_FALSE(expand_unicode_codepoint_escapes("u+110000", expanded));
	EXPECT_FALSE(expand_unicode_codepoint_escapes("U+D800", expanded));
}

// find() is the case-sensitive primitive the search engine relies on for its
// "Match case" path (search_replace_engine.cpp:163). It must be byte-exact and
// must NOT fold ASCII case.
TEST(lagi_string_utils, find_is_case_sensitive_ascii) {
	EXPECT_EQ(0u, find("Foo", "Foo"));
	EXPECT_EQ(4u, find("abc Foo xyz", "Foo"));

	// Differing case must not match.
	EXPECT_EQ(npos, find("foo", "Foo"));
	EXPECT_EQ(npos, find("FOO", "Foo"));
	EXPECT_EQ(npos, find("Foo", "foo"));
}

TEST(lagi_string_utils, find_handles_empty_and_missing) {
	// Empty needle: matches at the starting position (mirrors string_view::find).
	EXPECT_EQ(0u, find("Foo", ""));
	EXPECT_EQ(2u, find("Foo", "", 2));
	EXPECT_EQ(npos, find("", "Foo"));
	EXPECT_EQ(npos, find("abc", "Foo"));
}

// Regression: utf8_find_icase(view, view) previously fell back to a
// case-sensitive find() when StringZilla was disabled, which silently broke
// the search engine's case-insensitive path. This pins its behavior in both
// build configurations.
TEST(lagi_string_utils, utf8_find_icase_is_case_insensitive_ascii) {
	expect_same_match(utf8_find_icase("Foo Bar", "foo"), sized_match{0, 3});
	expect_same_match(utf8_find_icase("Foo Bar", "BAR"), sized_match{4, 3});
	expect_same_match(utf8_find_icase("FOO", "foo"), sized_match{0, 3});
	expect_same_match(utf8_find_icase("foo", "FOO"), sized_match{0, 3});
	expect_same_match(utf8_find_icase("Hello World", "xyz"), sized_match{});
}

// Regression: the cached utf8_icase_searcher path had the same broken fallback.
TEST(lagi_string_utils, utf8_icase_searcher_is_case_insensitive_ascii) {
	utf8_icase_searcher searcher("Foo");

	expect_same_match(utf8_find_icase("Hello Foo Bar", searcher), sized_match{6, 3});
	expect_same_match(utf8_find_icase("FOO at start", searcher), sized_match{0, 3});
	expect_same_match(utf8_find_icase("ends with foo", searcher), sized_match{10, 3});
	expect_same_match(utf8_find_icase("no match here", searcher), sized_match{});
}
