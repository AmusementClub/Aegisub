#include "selection_navigation_history.h"

#include <cstddef>
#include <deque>

namespace aegisub::selection_navigation_history {
namespace {
constexpr std::size_t max_history_entries = 128;

void push_history(std::deque<int>& history, int line_id) {
	if (line_id <= 0)
		return;
	if (!history.empty() && history.back() == line_id)
		return;

	history.push_back(line_id);
	if (history.size() > max_history_entries)
		history.pop_front();
}

bool can_go(std::deque<int> const& source, int current_line_id, History::LineIdIsValid const& is_valid) {
	for (auto it = source.rbegin(); it != source.rend(); ++it) {
		if (*it != current_line_id && is_valid(*it))
			return true;
	}
	return false;
}

std::optional<int> go(std::deque<int>& source, std::deque<int>& destination, int current_line_id, History::LineIdIsValid const& is_valid) {
	while (!source.empty()) {
		int const target_line_id = source.back();
		source.pop_back();
		if (target_line_id == current_line_id || !is_valid(target_line_id))
			continue;

		push_history(destination, current_line_id);
		return target_line_id;
	}

	return std::nullopt;
}
}

void History::Clear() {
	back_stack.clear();
	forward_stack.clear();
}

void History::RecordTransition(int previous_line_id, int current_line_id) {
	if (previous_line_id <= 0 || previous_line_id == current_line_id)
		return;

	push_history(back_stack, previous_line_id);
	forward_stack.clear();
}

void History::RecordVisit(int line_id) {
	push_history(back_stack, line_id);
}

bool History::CanGoBack(int current_line_id, LineIdIsValid const& is_valid) const {
	return can_go(back_stack, current_line_id, is_valid);
}

bool History::CanGoForward(int current_line_id, LineIdIsValid const& is_valid) const {
	return can_go(forward_stack, current_line_id, is_valid);
}

std::optional<int> History::GoBack(int current_line_id, LineIdIsValid const& is_valid) {
	return go(back_stack, forward_stack, current_line_id, is_valid);
}

std::optional<int> History::GoForward(int current_line_id, LineIdIsValid const& is_valid) {
	return go(forward_stack, back_stack, current_line_id, is_valid);
}

}
