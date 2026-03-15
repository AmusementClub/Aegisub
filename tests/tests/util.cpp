// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include <libaegisub/util.h>
#include <libaegisub/string_utils.h>

#include <main.h>

class lagi_util : public libagi { };

namespace agi {
TEST(lagi_util, try_parse_double) {
	double d = 0.0;
	EXPECT_TRUE(util::try_parse("1.0", &d));
	EXPECT_EQ(1.0, d);

	EXPECT_FALSE(util::try_parse("aaa", &d));
	EXPECT_EQ(1.0, d);

	EXPECT_FALSE(util::try_parse("2aaa", &d));
	EXPECT_EQ(1.0, d);
}

TEST(lagi_util, try_parse_int) {
	int i = 0;
	EXPECT_TRUE(util::try_parse("1", &i));
	EXPECT_EQ(1, i);

	EXPECT_FALSE(util::try_parse("2.0", &i));
	EXPECT_EQ(1.0, i);
}

TEST(lagi_util, split_any_skips_empty_segments) {
	std::vector<std::string> parts;
	util::strings::for_each_split_any("\r\nfirst\n\nsecond\rthird\r\n", "\r\n", [&](util::strings::view part) {
		parts.emplace_back(part);
	});

	ASSERT_EQ(3u, parts.size());
	EXPECT_EQ("first", parts[0]);
	EXPECT_EQ("second", parts[1]);
	EXPECT_EQ("third", parts[2]);
}

TEST(lagi_util, split_any_handles_no_delimiters) {
	std::vector<std::string> parts;
	util::strings::for_each_split_any("single", "\r\n", [&](util::strings::view part) {
		parts.emplace_back(part);
	});

	ASSERT_EQ(1u, parts.size());
	EXPECT_EQ("single", parts[0]);
}

TEST(lagi_util, split_any_preserves_empty_segments_when_requested) {
	auto parts = util::strings::split_any("16::9", ":", false);
	ASSERT_EQ(3u, parts.size());
	EXPECT_EQ("16", parts[0]);
	EXPECT_EQ("", parts[1]);
	EXPECT_EQ("9", parts[2]);
}

TEST(lagi_util, join_strings) {
	std::vector<std::string> parts = {"*.ass", "*.ssa", "*.srt"};
	EXPECT_EQ("*.ass,*.ssa,*.srt", util::strings::join(parts, ","));
	EXPECT_EQ("*.ass;*.ssa;*.srt", util::strings::join(parts, ";"));
}

}
