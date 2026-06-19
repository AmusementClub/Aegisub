#pragma once

#include <deque>
#include <functional>
#include <optional>

namespace aegisub::selection_navigation_history {

class History {
	std::deque<int> back_stack;
	std::deque<int> forward_stack;

public:
	using LineIdIsValid = std::function<bool(int)>;

	void Clear();
	void RecordTransition(int previous_line_id, int current_line_id);
	void RecordVisit(int line_id);

	bool CanGoBack(int current_line_id, LineIdIsValid const& is_valid) const;
	bool CanGoForward(int current_line_id, LineIdIsValid const& is_valid) const;

	std::optional<int> GoBack(int current_line_id, LineIdIsValid const& is_valid);
	std::optional<int> GoForward(int current_line_id, LineIdIsValid const& is_valid);
};

}
