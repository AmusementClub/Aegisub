#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aegisub::locale_pick {

std::optional<std::string> ResolveImmediateLanguage(
	std::vector<std::string> const& available_languages,
	std::string const& active_language,
	std::string const& preferred_language);

std::vector<std::string> BuildSelectionLanguages(
	std::vector<std::string> const& available_languages,
	std::string const& preferred_language);

}
