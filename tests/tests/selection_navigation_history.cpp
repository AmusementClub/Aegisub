#include <main.h>

#include "../../src/selection_navigation_history.h"

#include <set>
#include <utility>

namespace history = aegisub::selection_navigation_history;

namespace {
auto all_lines_live() {
	return [](int) { return true; };
}

auto only_live(std::set<int> live_lines) {
	return [live_lines = std::move(live_lines)](int line_id) {
		return live_lines.count(line_id) != 0;
	};
}
}

TEST(selection_navigation_history, goes_back_and_forward_through_active_line_transitions) {
	history::History subject;
	subject.RecordTransition(1, 2);
	subject.RecordTransition(2, 3);

	ASSERT_TRUE(subject.CanGoBack(3, all_lines_live()));
	auto back = subject.GoBack(3, all_lines_live());
	ASSERT_TRUE(back);
	EXPECT_EQ(2, *back);

	ASSERT_TRUE(subject.CanGoForward(2, all_lines_live()));
	auto forward = subject.GoForward(2, all_lines_live());
	ASSERT_TRUE(forward);
	EXPECT_EQ(3, *forward);
}

TEST(selection_navigation_history, new_transition_clears_forward_stack) {
	history::History subject;
	subject.RecordTransition(1, 2);
	subject.RecordTransition(2, 3);
	auto back = subject.GoBack(3, all_lines_live());
	ASSERT_TRUE(back);
	ASSERT_EQ(2, *back);

	subject.RecordTransition(2, 4);

	EXPECT_FALSE(subject.CanGoForward(4, all_lines_live()));
}

TEST(selection_navigation_history, skips_current_and_missing_lines_when_navigating_back) {
	history::History subject;
	subject.RecordTransition(1, 2);
	subject.RecordTransition(2, 3);
	subject.RecordVisit(3);

	ASSERT_TRUE(subject.CanGoBack(3, only_live({1, 3})));
	auto back = subject.GoBack(3, only_live({1, 3}));
	ASSERT_TRUE(back);
	EXPECT_EQ(1, *back);
}

TEST(selection_navigation_history, recording_edited_line_does_not_clear_forward_stack) {
	history::History subject;
	subject.RecordTransition(1, 2);
	subject.RecordTransition(2, 3);
	auto back = subject.GoBack(3, all_lines_live());
	ASSERT_TRUE(back);
	ASSERT_EQ(2, *back);

	subject.RecordVisit(2);

	ASSERT_TRUE(subject.CanGoForward(2, all_lines_live()));
	auto forward = subject.GoForward(2, all_lines_live());
	ASSERT_TRUE(forward);
	EXPECT_EQ(3, *forward);
}
