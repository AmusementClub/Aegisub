#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_style.h"
#include "../../src/font_name_normalization.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace {

FontFamilyRecord family(
	FontFamilyId id,
	std::string localized,
	std::string english,
	std::vector<FontFamilyName> names = {})
{
	FontFamilyRecord record;
	record.id = id;
	record.localized_family_name = std::move(localized);
	record.english_win32_family_name = std::move(english);
	record.names = std::move(names);
	return record;
}

AssStyle& add_style(AssFile& file, std::string name, std::string font) {
	auto *style = new AssStyle;
	style->name = std::move(name);
	style->font = std::move(font);
	file.Styles.push_back(*style);
	return *style;
}

AssDialogue& add_event(AssFile& file, std::string text, int row, bool comment = false) {
	auto *event = new AssDialogue;
	event->Text = std::move(text);
	event->Style = "Default";
	event->Row = row;
	event->Comment = comment;
	file.Events.push_back(*event);
	return *event;
}

} // namespace

TEST(font_name_normalization, reports_safe_style_and_override_changes_without_modifying_file) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family")});
	AssFile file;
	add_style(file, "Default", "Localized Family");
	auto& dialogue = add_event(
		file,
		"{\\fn}{\\fnLocalized Family}a{\\fnEnglish Family}b"
		"{\\t(0,100,\\fnLocalized Family)}c",
		40);
	add_event(file, "{\\fnLocalized Family}comment", 41, true);
	auto const original_text = dialogue.Text.get();

	std::array<int, 1> const style_source_lines{7};
	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32, style_source_lines);

	EXPECT_TRUE(plan.catalog_available);
	EXPECT_EQ(5u, plan.scanned_name_count);
	ASSERT_EQ(4u, plan.changes.size());
	EXPECT_EQ(FontNameSourceKind::Style, plan.changes[0].source.kind);
	EXPECT_EQ("Default", plan.changes[0].source.style);
	EXPECT_EQ(0u, plan.changes[0].source.entry_index);
	EXPECT_EQ(7, plan.changes[0].source.line);
	EXPECT_EQ("English Family", plan.changes[0].recommended_name);
	EXPECT_TRUE(plan.changes[0].safe_to_apply);
	EXPECT_EQ("localized_win32_family_alias", plan.changes[0].reason_code);
	EXPECT_EQ(41, plan.changes[1].source.line);
	EXPECT_EQ(FontNameSourceKind::Override, plan.changes[1].source.kind);
	EXPECT_EQ(0u, plan.changes[1].source.entry_index);
	EXPECT_EQ(1u, plan.changes[1].source.override_index);
	EXPECT_EQ(3u, plan.changes[2].source.override_index);
	EXPECT_TRUE(plan.changes[3].source.comment);
	EXPECT_EQ(1u, plan.changes[3].source.entry_index);
	EXPECT_EQ(original_text, dialogue.Text.get());
}

TEST(font_name_normalization, separates_case_repairs_from_informational_name_diagnostics) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family", {
		{"Other Locale Family", "zh-TW", FontFamilyNameKind::Win32Family},
		{"Full Face Name", "en-US", FontFamilyNameKind::FullName},
	})});
	AssFile file;
	add_style(file, "Default", "english family");
	add_event(file, "{\\fnFull Face Name}x{\\fnOther Locale Family}y", 10);

	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);

	ASSERT_EQ(3u, plan.changes.size());
	EXPECT_TRUE(plan.changes[0].safe_to_apply);
	EXPECT_EQ("English Family", plan.changes[0].recommended_name);
	EXPECT_EQ("noncanonical_family_name_case", plan.changes[0].reason_code);
	EXPECT_FALSE(plan.changes[1].safe_to_apply);
	EXPECT_EQ(FontFamilyMatchKind::None, plan.changes[1].match_kind);
	EXPECT_EQ("unrecognized_family_name", plan.changes[1].reason_code);
	EXPECT_TRUE(plan.changes[2].safe_to_apply);
	EXPECT_EQ("English Family", plan.changes[2].recommended_name);
	EXPECT_EQ("win32_family_alias", plan.changes[2].reason_code);
}

TEST(font_name_normalization, reports_unavailable_and_overlong_english_names_as_unsafe) {
	auto long_name = std::string(31, 'E');
	FontFamilyCatalog catalog({
		family(1, "Localized Only", ""),
		family(2, "Vertical Local", long_name),
	});
	AssFile file;
	add_style(file, "NoEnglish", "Localized Only");
	add_style(file, "Vertical", "@Vertical Local");

	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);

	ASSERT_EQ(2u, plan.changes.size());
	EXPECT_FALSE(plan.changes[0].safe_to_apply);
	EXPECT_EQ("english_win32_name_unavailable", plan.changes[0].reason_code);
	EXPECT_FALSE(plan.changes[1].safe_to_apply);
	EXPECT_EQ("@" + long_name, plan.changes[1].recommended_name);
	EXPECT_EQ("gdi_family_name_too_long", plan.changes[1].reason_code);
}

TEST(font_name_normalization, applies_selected_style_and_nested_override_changes) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family")});
	AssFile file;
	add_style(file, "Default", "Localized Family");
	auto& dialogue = add_event(
		file, "{\\fnLocalized Family}a{\\t(0,100,\\fnLocalized Family)}b", 4);

	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);
	ASSERT_EQ(3u, plan.changes.size());
	std::array<std::size_t, 3> const selected{0, 1, 2};

	auto result = ApplyFontNameNormalizationChanges(file, plan, selected);

	EXPECT_TRUE(result.success) << result.error;
	EXPECT_TRUE(result.styles_changed);
	EXPECT_TRUE(result.dialogue_text_changed);
	EXPECT_EQ(3u, result.applied_change_count);
	EXPECT_EQ("English Family", file.Styles.front().font);
	EXPECT_EQ("{\\fnEnglish Family}a{\\t(0,100,\\fnEnglish Family)}b", dialogue.Text.get());

	auto after = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);
	EXPECT_TRUE(after.changes.empty());
}

TEST(font_name_normalization, stale_source_rejects_all_changes_before_mutation) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family")});
	AssFile file;
	add_style(file, "Default", "Localized Family");
	auto& dialogue = add_event(file, "{\\fnLocalized Family}x", 7);
	auto const original_text = dialogue.Text.get();

	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);
	ASSERT_EQ(2u, plan.changes.size());
	file.Styles.front().font = "Changed Elsewhere";
	std::array<std::size_t, 2> const selected{0, 1};

	auto result = ApplyFontNameNormalizationChanges(file, plan, selected);

	EXPECT_FALSE(result.success);
	EXPECT_EQ(0u, result.applied_change_count);
	EXPECT_EQ(original_text, dialogue.Text.get());
}

TEST(font_name_normalization, stale_override_rejects_style_change_before_mutation) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family")});
	AssFile file;
	add_style(file, "Default", "Localized Family");
	auto& dialogue = add_event(file, "{\\fnLocalized Family}x", 7);

	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);
	ASSERT_EQ(2u, plan.changes.size());
	dialogue.Text = "{\\fnChanged Elsewhere}x";
	std::array<std::size_t, 2> const selected{0, 1};

	auto result = ApplyFontNameNormalizationChanges(file, plan, selected);

	EXPECT_FALSE(result.success);
	EXPECT_EQ(0u, result.applied_change_count);
	EXPECT_EQ("Localized Family", file.Styles.front().font);
	EXPECT_EQ("{\\fnChanged Elsewhere}x", dialogue.Text.get());
}

TEST(font_name_normalization, rejects_unsafe_findings) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family")});
	AssFile file;
	add_style(file, "Default", "Unknown Family");
	auto plan = BuildFontNameNormalizationPlan(
		file, catalog, FontNameNormalizationTarget::EnglishWin32);
	ASSERT_EQ(1u, plan.changes.size());
	ASSERT_FALSE(plan.changes[0].safe_to_apply);
	std::array<std::size_t, 1> const selected{0};

	auto result = ApplyFontNameNormalizationChanges(file, plan, selected);

	EXPECT_FALSE(result.success);
	EXPECT_EQ("Unknown Family", file.Styles.front().font);
}
