#pragma once

#include <string>
#include <utility>
#include <vector>

namespace aegisub::hotkey_migration {

template<typename HotkeyMap>
void RenameCommand(
	HotkeyMap& hotkeys,
	std::string const& old_command,
	std::string const& new_command) {
	auto const range = hotkeys.equal_range(old_command);
	std::vector<typename HotkeyMap::mapped_type> replacements;
	for (auto it = range.first; it != range.second; ++it)
		replacements.emplace_back(it->second.Context(), new_command, it->second.Str());

	hotkeys.erase(range.first, range.second);
	for (auto& combo : replacements) {
		auto command = combo.CmdName();
		hotkeys.emplace(std::move(command), std::move(combo));
	}
}

}
