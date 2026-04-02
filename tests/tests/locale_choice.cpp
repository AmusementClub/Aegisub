#include "../../src/locale_choice.h"

#include <gtest/gtest.h>

TEST(locale_choice, build_request_uses_stable_request_id_and_choices) {
	std::vector<std::string> const languages = {"en_US", "ja", "zh_CN"};

	auto request = aegisub::locale_choice::BuildRequest(languages);

	EXPECT_EQ("locale_choice.ui_language", request.request_id);
	EXPECT_EQ(languages, request.choices);
	EXPECT_FALSE(request.title.empty());
	EXPECT_FALSE(request.message.empty());
}

TEST(locale_choice, resolve_selection_returns_selected_language) {
	std::vector<std::string> const languages = {"en_US", "ja", "zh_CN"};

	auto selected = aegisub::locale_choice::ResolveSelection(languages, 1);

	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ("ja", *selected);
}

TEST(locale_choice, resolve_selection_rejects_cancel_and_out_of_range) {
	std::vector<std::string> const languages = {"en_US", "ja", "zh_CN"};

	EXPECT_EQ(std::nullopt, aegisub::locale_choice::ResolveSelection(languages, std::nullopt));
	EXPECT_EQ(std::nullopt, aegisub::locale_choice::ResolveSelection(languages, -1));
	EXPECT_EQ(std::nullopt, aegisub::locale_choice::ResolveSelection(languages, 3));
}
