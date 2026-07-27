#include <main.h>

#include "../../src/single_choice_layout.h"

using aegisub::single_choice_layout::kRadioListMaxRows;
using aegisub::single_choice_layout::kScrollableListMaxVisibleRows;
using aegisub::single_choice_layout::kScrollableListMinVisibleRows;
using aegisub::single_choice_layout::PlanLayout;
using aegisub::single_choice_layout::Presentation;

TEST(single_choice_layout, at_or_below_threshold_uses_radio_list) {
	auto plan = PlanLayout(kRadioListMaxRows);

	EXPECT_EQ(Presentation::RadioList, plan.presentation);
	EXPECT_EQ(0, plan.visible_rows);

	plan = PlanLayout(1);
	EXPECT_EQ(Presentation::RadioList, plan.presentation);

	plan = PlanLayout(kRadioListMaxRows - 1);
	EXPECT_EQ(Presentation::RadioList, plan.presentation);
}

TEST(single_choice_layout, above_threshold_uses_scrollable_list) {
	auto plan = PlanLayout(kRadioListMaxRows + 1);

	EXPECT_EQ(Presentation::ScrollableList, plan.presentation);
	EXPECT_EQ(static_cast<int>(kRadioListMaxRows + 1), plan.visible_rows);
}

TEST(single_choice_layout, visible_rows_clamped_to_upper_bound) {
	auto plan = PlanLayout(50);

	EXPECT_EQ(Presentation::ScrollableList, plan.presentation);
	EXPECT_EQ(kScrollableListMaxVisibleRows, plan.visible_rows);
}

TEST(single_choice_layout, scrollable_visible_rows_stay_inside_clamp_window) {
	EXPECT_LE(kScrollableListMinVisibleRows, kScrollableListMaxVisibleRows);

	// Sweep across the threshold so the invariant holds for every count rather
	// than pinning the specific constant values.
	for (size_t count = 1; count <= 64; ++count) {
		auto plan = PlanLayout(count);
		if (count <= kRadioListMaxRows) {
			EXPECT_EQ(Presentation::RadioList, plan.presentation) << count;
			continue;
		}

		EXPECT_EQ(Presentation::ScrollableList, plan.presentation) << count;
		EXPECT_GE(plan.visible_rows, kScrollableListMinVisibleRows) << count;
		EXPECT_LE(plan.visible_rows, kScrollableListMaxVisibleRows) << count;
		// Never claim more visible rows than there are choices to show.
		EXPECT_LE(static_cast<size_t>(plan.visible_rows), count) << count;
	}
}

TEST(single_choice_layout, empty_choice_count_is_safe) {
	// Callers short-circuit empty lists before showing a dialog; PlanLayout
	// must still return a defined radio presentation without crashing.
	auto plan = PlanLayout(0);

	EXPECT_EQ(Presentation::RadioList, plan.presentation);
	EXPECT_EQ(0, plan.visible_rows);
}
