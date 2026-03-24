#pragma once

#include "ui_services.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::track_choice {

enum class DialogKind {
	Audio,
	Subtitle,
	Video,
};

agi::SingleChoiceInteractionRequest BuildRequest(DialogKind kind, std::vector<std::string> const& choices);
std::optional<int> ResolveSelection(size_t choice_count, std::optional<int> selection);

}
