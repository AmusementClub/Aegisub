#include "locale_choice.h"

namespace aegisub::locale_choice {

agi::SingleChoiceInteractionRequest BuildRequest(std::vector<std::string> const& languages) {
	agi::SingleChoiceInteractionRequest request;
	request.title = "Language";
	request.message = "Please choose a language:";
	request.choices = languages;
	request.request_id = "locale_choice.ui_language";
	return request;
}

std::optional<std::string> ResolveSelection(std::vector<std::string> const& languages, std::optional<int> selection) {
	if (!selection)
		return std::nullopt;
	if (*selection < 0 || static_cast<size_t>(*selection) >= languages.size())
		return std::nullopt;
	return languages[*selection];
}

}
