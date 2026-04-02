#include "charset_choice.h"

namespace aegisub::charset_choice {

agi::SingleChoiceInteractionRequest BuildRequest(std::vector<std::string> const& choices) {
	agi::SingleChoiceInteractionRequest request;
	request.title = "Choose character set";
	request.message = "Aegisub could not narrow down the character set to a single one.\nPlease pick one below:";
	request.choices = choices;
	request.request_id = "charset_choice.detected_charsets";
	return request;
}

std::optional<std::string> ResolveSelection(std::vector<std::string> const& choices, std::optional<int> selection) {
	if (!selection)
		return std::nullopt;
	if (*selection < 0 || static_cast<size_t>(*selection) >= choices.size())
		return std::nullopt;
	return choices[*selection];
}

}
