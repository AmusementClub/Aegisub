#include "../../src/aegisublocale_compat.h"

#include <gtest/gtest.h>

TEST(aegisublocale_compat, maps_bcp47_chinese_tag_to_existing_posix_translation) {
	wxArrayString supported;
	supported.push_back(wxS("zh_CN"));
	supported.push_back(wxS("ja"));

	wxVector<wxString> preferred;
	preferred.push_back(wxS("zh-Hans-CN"));

	EXPECT_EQ(wxS("zh_CN"), aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, maps_bcp47_region_tag_to_existing_posix_translation) {
	wxArrayString supported;
	supported.push_back(wxS("pt_PT"));
	supported.push_back(wxS("pt_BR"));

	wxVector<wxString> preferred;
	preferred.push_back(wxS("pt-BR"));

	EXPECT_EQ(wxS("pt_BR"), aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, falls_through_to_next_preferred_language_when_earlier_ones_do_not_match) {
	wxArrayString supported;
	supported.push_back(wxS("ja"));

	wxVector<wxString> preferred;
	preferred.push_back(wxS("zh-Hans-CN"));
	preferred.push_back(wxS("en-US"));
	preferred.push_back(wxS("ja"));

	EXPECT_EQ(wxS("ja"), aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, maps_bcp47_script_tag_to_existing_posix_variant_translation) {
	wxArrayString supported;
	supported.push_back(wxS("sr_RS@latin"));
	supported.push_back(wxS("sr_RS"));

	wxVector<wxString> preferred;
	preferred.push_back(wxS("sr-Latn-RS"));

	EXPECT_EQ(wxS("sr_RS@latin"), aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, maps_traditional_chinese_script_tag_to_existing_posix_translation) {
	wxArrayString supported;
	supported.push_back(wxS("zh_TW"));
	supported.push_back(wxS("zh_CN"));

	wxVector<wxString> preferred;
	preferred.push_back(wxS("zh-Hant-TW"));

	EXPECT_EQ(wxS("zh_TW"), aegisub::locale::FindPreferredTranslation(supported, preferred));
}

TEST(aegisublocale_compat, falls_back_to_language_only_translation_when_region_specific_translation_is_missing) {
	wxArrayString supported;
	supported.push_back(wxS("pt"));

	wxVector<wxString> preferred;
	preferred.push_back(wxS("pt-BR"));

	EXPECT_EQ(wxS("pt"), aegisub::locale::FindPreferredTranslation(supported, preferred));
}
