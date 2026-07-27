#pragma once

#include <cstddef>

namespace aegisub::single_choice_layout {

// Max rows that still use a non-scrolling wxRadioBox.
inline constexpr size_t kRadioListMaxRows = 8;

// Visible row window for the scrollable list presentation.
inline constexpr int kScrollableListMinVisibleRows = 8;
inline constexpr int kScrollableListMaxVisibleRows = 12;

enum class Presentation {
	RadioList,
	ScrollableList,
};

struct LayoutPlan {
	Presentation presentation = Presentation::RadioList;
	// Meaningful for ScrollableList: preferred number of visible rows.
	int visible_rows = 0;
};

// Pure layout decision for single-choice dialogs. wx-free so unit tests and
// host_boundary_policy can cover it without expanding the wx adapter surface.
LayoutPlan PlanLayout(size_t choice_count);

}
