#include "charset_choice.h"

#include "compat.h"

#include <wx/intl.h>

namespace aegisub::charset_choice {

agi::SingleChoiceInteractionRequest BuildRequest(std::vector<std::string> const& choices) {
	agi::SingleChoiceInteractionRequest request;
	request.title = from_wx(_("Choose character set"));
	request.message = from_wx(_("Aegisub could not narrow down the character set to a single one.\nPlease pick one below:"));
	request.choices = choices;
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
