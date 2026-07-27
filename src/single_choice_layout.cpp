#include "single_choice_layout.h"

#include <algorithm>

namespace aegisub::single_choice_layout {

LayoutPlan PlanLayout(size_t choice_count) {
	if (choice_count <= kRadioListMaxRows)
		return {Presentation::RadioList, 0};

	int const rows = static_cast<int>(std::min(choice_count, static_cast<size_t>(kScrollableListMaxVisibleRows)));
	return {
		Presentation::ScrollableList,
		std::clamp(rows, kScrollableListMinVisibleRows, kScrollableListMaxVisibleRows),
	};
}

}
