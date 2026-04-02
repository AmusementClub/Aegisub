#include "../../src/locale_pick.h"

#include <gtest/gtest.h>

TEST(locale_pick, resolve_immediate_language_prefers_preferred_translation_for_first_run) {
	std::vector<std::string> const available = {"ja", "zh_CN"};

	auto language = aegisub::locale_pick::ResolveImmediateLanguage(available, "", "ja");

	ASSERT_TRUE(language.has_value());
	EXPECT_EQ("ja", *language);
}

TEST(locale_pick, resolve_immediate_language_falls_back_to_english_when_no_translations_exist) {
	auto language = aegisub::locale_pick::ResolveImmediateLanguage({}, "", "");

	ASSERT_TRUE(language.has_value());
	EXPECT_EQ("en_US", *language);
}

TEST(locale_pick, resolve_immediate_language_skips_auto_pick_when_language_is_already_active) {
	std::vector<std::string> const available = {"ja", "zh_CN"};

	EXPECT_EQ(std::nullopt, aegisub::locale_pick::ResolveImmediateLanguage(available, "zh_CN", "ja"));
}

TEST(locale_pick, build_selection_languages_inserts_english_and_rotates_preferred_to_front) {
	std::vector<std::string> const available = {"ja", "zh_CN"};

	auto languages = aegisub::locale_pick::BuildSelectionLanguages(available, "ja");

	std::vector<std::string> const expected = {"ja", "en_US", "zh_CN"};
	EXPECT_EQ(expected, languages);
}

TEST(locale_pick, build_selection_languages_does_not_duplicate_english) {
	std::vector<std::string> const available = {"en_US", "ja"};

	auto languages = aegisub::locale_pick::BuildSelectionLanguages(available, "");

	std::vector<std::string> const expected = {"en_US", "ja"};
	EXPECT_EQ(expected, languages);
}
