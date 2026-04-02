#pragma once

#include "ui_services.h"

#include <optional>
#include <string>
#include <vector>

namespace aegisub::charset_choice {

agi::SingleChoiceInteractionRequest BuildRequest(std::vector<std::string> const& choices);
std::optional<std::string> ResolveSelection(std::vector<std::string> const& choices, std::optional<int> selection);

}
