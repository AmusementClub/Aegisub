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

struct TrackLabel {
	int index = -1;
	std::string codec;
	std::vector<std::string> details;
	std::string title;
};

std::string FormatTrackLabel(TrackLabel const& label);
agi::SingleChoiceInteractionRequest BuildRequest(DialogKind kind, std::vector<std::string> const& choices);
// When loading an embedded track into the secondary subtitle strip, pass
// secondary = true so the dialog title/message identify it as such.
agi::SingleChoiceInteractionRequest BuildRequest(DialogKind kind, std::vector<std::string> const& choices, bool secondary);
std::optional<int> ResolveSelection(size_t choice_count, std::optional<int> selection);

}
