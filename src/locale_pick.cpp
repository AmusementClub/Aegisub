#include "locale_pick.h"

#include <algorithm>

namespace aegisub::locale_pick {
namespace {

bool ContainsLanguage(std::vector<std::string> const& languages, std::string const& language) {
	return std::find(languages.begin(), languages.end(), language) != languages.end();
}

}

std::optional<std::string> ResolveImmediateLanguage(
	std::vector<std::string> const& available_languages,
	std::string const& active_language,
	std::string const& preferred_language) {
	if (!active_language.empty())
		return std::nullopt;

	if (!preferred_language.empty() && ContainsLanguage(available_languages, preferred_language))
		return preferred_language;

	if (available_languages.empty())
		return std::string("en_US");

	return std::nullopt;
}

std::vector<std::string> BuildSelectionLanguages(
	std::vector<std::string> const& available_languages,
	std::string const& preferred_language) {
	std::vector<std::string> languages = available_languages;

	if (!ContainsLanguage(languages, "en_US"))
		languages.insert(languages.begin(), "en_US");

	if (!preferred_language.empty()) {
		auto it = std::find(languages.begin(), languages.end(), preferred_language);
		if (it != languages.end())
			std::rotate(languages.begin(), it, it + 1);
	}

	return languages;
}

}
