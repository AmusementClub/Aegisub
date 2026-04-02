#include "subtitle_grid_ops.h"

#include <algorithm>

namespace {

template<class T, class U>
bool move_one(T begin, T end, U const& to_move, bool swap) {
	size_t move_count = 0;
	auto prev = end;
	for (auto it = begin; it != end; ++it) {
		auto cur = &*it;
		if (!to_move.count(cur))
			prev = it;
		else if (prev != end) {
			it->swap_nodes(*prev);
			if (swap)
				std::swap(prev, it);
			else
				prev = it;
			if (++move_count == to_move.size())
				break;
		}
	}

	return move_count > 0;
}

}

namespace aegisub::subtitle_grid_ops {

std::unique_ptr<AssDialogue> CreateLineAfter(AssDialogue const& current, int default_duration) {
	auto line = std::make_unique<AssDialogue>();
	line->Start = current.End;
	line->End = current.End + default_duration;
	line->Style = current.Style;
	return line;
}

bool MoveSelectionUp(EntryList<AssDialogue>& events, Selection const& selection) {
	return move_one(events.begin(), events.end(), selection, false);
}

bool MoveSelectionDown(EntryList<AssDialogue>& events, Selection const& selection) {
	return move_one(events.rbegin(), events.rend(), selection, true);
}

bool SwapSelection(Selection const& selection) {
	if (selection.size() != 2)
		return false;

	(*selection.begin())->swap_nodes(**selection.rbegin());
	return true;
}

}
