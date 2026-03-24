#include <main.h>

#include "../../src/compat.h"

TEST(compat, string_roundtrip_preserves_utf8_content) {
	std::string utf8 =
		"\xE4\xB8\xAD\xE6\x96\x87"
		"/"
		"\xC3\xA9"
		"/"
		"\xF0\x9F\x99\x82";

	EXPECT_EQ(utf8, from_wx(to_wx(utf8)));
}

TEST(compat, vector_conversion_preserves_each_entry) {
	std::string utf8 =
		"\xE6\x96\x87"
		"\xE6\xA1\xA3";
	std::vector<std::string> input = {"plain", utf8, std::string()};

	auto values = to_wx(input);

	ASSERT_EQ(input.size(), values.size());
	for (size_t i = 0; i < input.size(); ++i)
		EXPECT_EQ(input[i], from_wx(values[i]));
}
