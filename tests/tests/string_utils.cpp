// Copyright (c) 2026
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

#include <libaegisub/string_utils.h>

#include <main.h>

#include <array>

namespace {
using agi::util::strings::sized_match;
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
