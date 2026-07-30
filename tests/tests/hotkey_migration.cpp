#include <main.h>

#include <libaegisub/hotkey.h>

#include "../../src/hotkey_migration.h"

namespace migration = aegisub::hotkey_migration;

TEST(hotkey_migration, rename_does_not_revisit_inserted_entries) {
	agi::hotkey::Hotkey::HotkeyMap hotkeys;
	hotkeys.emplace(
		"edit/line/duplicate/shift_back",
		agi::hotkey::Combo("Default", "edit/line/duplicate/shift_back", "Ctrl-Shift-D"));
	hotkeys.emplace(
		"grid/line/next",
		agi::hotkey::Combo("Default", "grid/line/next", "Ctrl-Shift-N"));

	migration::RenameCommand(
		hotkeys, "edit/line/duplicate/shift_back", "edit/line/split/after");

	EXPECT_EQ(2, hotkeys.size());
	EXPECT_EQ(0, hotkeys.count("edit/line/duplicate/shift_back"));
	ASSERT_EQ(1, hotkeys.count("edit/line/split/after"));
	EXPECT_EQ(1, hotkeys.count("grid/line/next"));

	auto const& migrated = hotkeys.find("edit/line/split/after")->second;
	EXPECT_EQ("Default", migrated.Context());
	EXPECT_EQ("Ctrl-Shift-D", migrated.Str());
}

TEST(hotkey_migration, rename_preserves_all_bindings) {
	agi::hotkey::Hotkey::HotkeyMap hotkeys;
	hotkeys.emplace("old", agi::hotkey::Combo("Default", "old", "Ctrl-D"));
	hotkeys.emplace("old", agi::hotkey::Combo("Subtitle Grid", "old", "Alt-D"));

	migration::RenameCommand(hotkeys, "old", "new");

	EXPECT_EQ(0, hotkeys.count("old"));
	ASSERT_EQ(2, hotkeys.count("new"));
	bool found_default = false;
	bool found_grid = false;
	auto const range = hotkeys.equal_range("new");
	for (auto it = range.first; it != range.second; ++it) {
		found_default |= it->second.Context() == "Default" && it->second.Str() == "Ctrl-D";
		found_grid |= it->second.Context() == "Subtitle Grid" && it->second.Str() == "Alt-D";
	}
	EXPECT_TRUE(found_default);
	EXPECT_TRUE(found_grid);
}
