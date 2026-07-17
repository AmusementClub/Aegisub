#include <gtest/gtest.h>

#include "../../src/font_face_selection.h"
#include "../../src/font_family_catalog.h"
#include "../../src/font_family_catalog_ui.h"
#ifdef _WIN32
#include "../../src/font_family_catalog_win_detail.h"
#endif

#include <string>
#include <memory>
#include <utility>
#include <vector>

namespace {

FontFamilyRecord family(
	FontFamilyId id,
	std::string localized,
	std::string english,
	std::vector<FontFamilyName> aliases = {})
{
	FontFamilyRecord record;
	record.id = id;
	record.localized_family_name = std::move(localized);
	record.english_win32_family_name = std::move(english);
	record.names = std::move(aliases);
	return record;
}

} // namespace

TEST(font_family_catalog, maps_localized_and_english_names_to_preferred_family_name) {
	FontFamilyCatalog catalog({family(1, "\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", "Microsoft YaHei")});

	auto localized = catalog.Resolve("\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91");
	ASSERT_EQ(FontFamilyMatchKind::Exact, localized.match);
	EXPECT_EQ(1u, localized.family);
	EXPECT_EQ("Microsoft YaHei", catalog.MapToPreferredWriteName(
		"\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", false));
	EXPECT_EQ("\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", catalog.MapToPreferredWriteName(
		"Microsoft YaHei", true));
	EXPECT_EQ("Microsoft YaHei", catalog.MapToPreferredWriteName(
		"microsoft yahei", false));
}

TEST(font_family_catalog, falls_back_to_localized_name_when_english_name_is_unavailable) {
	FontFamilyCatalog catalog({family(1, "Localized Only", "")});

	EXPECT_EQ("Localized Only", catalog.PreferredWriteName(1, false));
	EXPECT_EQ("Localized Only", catalog.MapToPreferredWriteName("Localized Only", false));
	EXPECT_EQ(std::vector<std::string>{"Localized Only"}, catalog.DisplayNames(false));
}

TEST(font_family_catalog, leaves_ambiguous_alias_unchanged) {
	FontFamilyName alias{"Shared Alias", "en-US", FontFamilyNameKind::Win32Family};
	FontFamilyCatalog catalog({
		family(1, "Localized A", "English A", {alias}),
		family(2, "Localized B", "English B", {alias}),
	});

	auto result = catalog.Resolve("Shared Alias");
	EXPECT_EQ(FontFamilyMatchKind::Ambiguous, result.match);
	EXPECT_FALSE(result.family.has_value());
	EXPECT_EQ("Shared Alias", catalog.MapToPreferredWriteName("Shared Alias", false));
}

TEST(font_family_catalog, exact_alias_wins_before_case_insensitive_ambiguity) {
	FontFamilyCatalog catalog({
		family(1, "Localized A", "Alias"),
		family(2, "Localized B", "ALIAS"),
	});

	EXPECT_EQ(1u, catalog.Resolve("Alias").family);
	EXPECT_EQ(2u, catalog.Resolve("ALIAS").family);
	EXPECT_EQ(FontFamilyMatchKind::Ambiguous, catalog.Resolve("alias").match);
}

TEST(font_family_catalog, indexes_only_win32_family_names_as_ass_aliases) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family", {
		{"Win32 Alias", "en-US", FontFamilyNameKind::Win32Family},
		{"Typographic Family", "en-US", FontFamilyNameKind::TypographicFamily},
		{"Full Face Name", "en-US", FontFamilyNameKind::FullName},
		{"PostScript-Name", "en-US", FontFamilyNameKind::PostScript},
		{"Platform Alias", "", FontFamilyNameKind::PlatformAlias},
	})});

	EXPECT_EQ(FontFamilyMatchKind::Exact, catalog.Resolve("Win32 Alias").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Typographic Family").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Full Face Name").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("PostScript-Name").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Platform Alias").match);
}

TEST(font_family_catalog, vertical_prefix_counts_toward_gdi_limit) {
	auto thirty_one = std::string(31, 'A');
	auto thirty = std::string(30, 'B');
	FontFamilyCatalog catalog({
		family(1, "Local31", thirty_one),
		family(2, "Local30", thirty),
	});

	EXPECT_EQ(thirty_one, catalog.MapToPreferredWriteName("Local31", false));
	EXPECT_EQ("@Local31", catalog.MapToPreferredWriteName("@Local31", false));
	EXPECT_EQ("@" + thirty, catalog.MapToPreferredWriteName("@Local30", false));
	EXPECT_EQ(31u, FontFamilyCatalog::Utf16CodeUnitLength("@" + thirty));
	EXPECT_EQ(2u, FontFamilyCatalog::Utf16CodeUnitLength("\xF0\x9F\x98\x80"));
}

TEST(font_face_selection, writes_displayed_name_for_explicit_alias_but_not_unchanged_inherited_font) {
	EXPECT_TRUE(ShouldWriteFontFace(
		"Localized Family", "English Family", true, "English Family"));
	EXPECT_FALSE(ShouldWriteFontFace(
		"Localized Family", "English Family", false, "English Family"));
	EXPECT_TRUE(ShouldWriteFontFace(
		"Arial", "Arial", true, "English Family"));
	EXPECT_FALSE(ShouldWriteFontFace(
		"English Family", "English Family", true, "English Family"));
}

TEST(font_family_catalog_ui, displayed_name_follows_preference_for_existing_style) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{family(1, "Localized Family", "English Family")});
	FontFamilyCatalogUiModel model;
	model.catalog = catalog;

	model.prefer_localized = false;
	EXPECT_EQ("English Family", model.PreferredName("Localized Family"));
	model.prefer_localized = true;
	EXPECT_EQ("Localized Family", model.PreferredName("English Family"));
}

#ifdef _WIN32
TEST(font_family_catalog_win, rejects_same_name_candidate_resolving_to_different_entity) {
	using font_family_catalog_win_detail::FontEntityKey;
	using font_family_catalog_win_detail::SameEntity;

	FontEntityKey localized{"collection.ttc", 0, true};
	FontEntityKey same{"collection.ttc", 0, true};
	FontEntityKey other_face{"collection.ttc", 1, true};
	FontEntityKey other_file{"substitute.ttf", 0, true};

	EXPECT_TRUE(SameEntity(localized, same));
	EXPECT_FALSE(SameEntity(localized, other_face));
	EXPECT_FALSE(SameEntity(localized, other_file));
	EXPECT_FALSE(SameEntity(localized, FontEntityKey{}));
}
#endif
