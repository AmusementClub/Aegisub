#include <main.h>

#include "../../src/charset_choice.h"

TEST(charset_choice, build_request_preserves_choices) {
	std::vector<std::string> choices = {"utf-8", "shift_jis"};

	auto request = aegisub::charset_choice::BuildRequest(choices);

	EXPECT_EQ(choices, request.choices);
	EXPECT_FALSE(request.title.empty());
	EXPECT_FALSE(request.message.empty());
}

TEST(charset_choice, resolve_selection_returns_selected_value) {
	std::vector<std::string> choices = {"utf-8", "gb18030"};

	auto selected = aegisub::charset_choice::ResolveSelection(choices, 1);

	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ("gb18030", *selected);
}

TEST(charset_choice, resolve_selection_rejects_cancel_and_out_of_range) {
	std::vector<std::string> choices = {"utf-8"};

	EXPECT_EQ(std::nullopt, aegisub::charset_choice::ResolveSelection(choices, std::nullopt));
	EXPECT_EQ(std::nullopt, aegisub::charset_choice::ResolveSelection(choices, -1));
	EXPECT_EQ(std::nullopt, aegisub::charset_choice::ResolveSelection(choices, 3));
}
