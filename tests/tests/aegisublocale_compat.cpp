#include "../../src/aegisublocale_compat.h"

#include <gtest/gtest.h>

TEST(aegisublocale_compat, maps_bcp47_chinese_tag_to_existing_posix_translation) {
	wxArrayString supported;
	supported.push_back("zh_CN");
	supported.push_back("ja");

	wxVector<wxString> preferred;
	preferred.push_back("zh-Hans-CN");

	EXPECT_EQ("zh_CN", aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, maps_bcp47_region_tag_to_existing_posix_translation) {
	wxArrayString supported;
	supported.push_back("pt_PT");
	supported.push_back("pt_BR");

	wxVector<wxString> preferred;
	preferred.push_back("pt-BR");

	EXPECT_EQ("pt_BR", aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, falls_through_to_next_preferred_language_when_earlier_ones_do_not_match) {
	wxArrayString supported;
	supported.push_back("ja");

	wxVector<wxString> preferred;
	preferred.push_back("zh-Hans-CN");
	preferred.push_back("en-US");
	preferred.push_back("ja");

	EXPECT_EQ("ja", aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, maps_bcp47_script_tag_to_existing_posix_variant_translation) {
	wxArrayString supported;
	supported.push_back("sr_RS@latin");
	supported.push_back("sr_RS");

	wxVector<wxString> preferred;
	preferred.push_back("sr-Latn-RS");

	EXPECT_EQ("sr_RS@latin", aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, maps_traditional_chinese_script_tag_to_existing_posix_translation) {
	wxArrayString supported;
	supported.push_back("zh_TW");
	supported.push_back("zh_CN");

	wxVector<wxString> preferred;
	preferred.push_back("zh-Hant-TW");

	EXPECT_EQ("zh_TW", aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, falls_back_to_language_only_translation_when_region_specific_translation_is_missing) {
	wxArrayString supported;
	supported.push_back("pt");

	wxVector<wxString> preferred;
	preferred.push_back("pt-BR");

	EXPECT_EQ("pt", aegisub::locale::FindPreferredTranslation(supported, preferred));
}
