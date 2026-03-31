#pragma once

#include "ui_services.h"

#include <optional>
#include <string>
#include <vector>

namespace aegisub::locale_choice {

agi::SingleChoiceInteractionRequest BuildRequest(std::vector<std::string> const& languages);
std::optional<std::string> ResolveSelection(std::vector<std::string> const& languages, std::optional<int> selection);

}
