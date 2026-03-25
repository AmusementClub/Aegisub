#pragma once

#include "ass_dialogue.h"
#include "ass_file.h"
#include "selection_controller.h"

#include <memory>

namespace aegisub::subtitle_grid_ops {

std::unique_ptr<AssDialogue> CreateLineAfter(AssDialogue const& current, int default_duration);
bool MoveSelectionUp(EntryList<AssDialogue>& events, Selection const& selection);
bool MoveSelectionDown(EntryList<AssDialogue>& events, Selection const& selection);
bool SwapSelection(Selection const& selection);

}
