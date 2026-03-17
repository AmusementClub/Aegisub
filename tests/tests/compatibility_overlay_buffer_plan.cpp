#include <main.h>

#include "../../src/compatibility_overlay_buffer_plan.h"

TEST(compatibility_overlay_buffer_plan, prefers_requested_slot_when_it_is_available) {
	std::array<CompatibilityOverlayBufferSlotState, 2> slots = { };
	slots[0].allocated = true;
	slots[0].reusable = true;
	slots[1].allocated = true;
	slots[1].reusable = true;

	auto plan = DecideCompatibilityOverlayBufferPlan(0, slots);
	EXPECT_EQ(CompatibilityOverlayBufferPlanAction::UseSlot0, plan.action);
	EXPECT_EQ(1, plan.next_preferred_slot);
}

TEST(compatibility_overlay_buffer_plan, skips_previous_slot_and_uses_other_reusable_slot) {
	std::array<CompatibilityOverlayBufferSlotState, 2> slots = { };
	slots[0].allocated = true;
	slots[0].reusable = true;
	slots[0].holds_previous = true;
	slots[1].allocated = true;
	slots[1].reusable = true;

	auto plan = DecideCompatibilityOverlayBufferPlan(0, slots);
	EXPECT_EQ(CompatibilityOverlayBufferPlanAction::UseSlot1, plan.action);
	EXPECT_EQ(0, plan.next_preferred_slot);
}

TEST(compatibility_overlay_buffer_plan, chooses_unallocated_slot_before_overflow) {
	std::array<CompatibilityOverlayBufferSlotState, 2> slots = { };
	slots[0].allocated = true;
	slots[0].reusable = false;
	slots[0].holds_previous = true;

	auto plan = DecideCompatibilityOverlayBufferPlan(0, slots);
	EXPECT_EQ(CompatibilityOverlayBufferPlanAction::UseSlot1, plan.action);
	EXPECT_EQ(0, plan.next_preferred_slot);
}

TEST(compatibility_overlay_buffer_plan, falls_back_to_overflow_when_both_slots_are_busy) {
	std::array<CompatibilityOverlayBufferSlotState, 2> slots = { };
	slots[0].allocated = true;
	slots[0].reusable = false;
	slots[0].holds_previous = true;
	slots[1].allocated = true;
	slots[1].reusable = false;

	auto plan = DecideCompatibilityOverlayBufferPlan(0, slots);
	EXPECT_EQ(CompatibilityOverlayBufferPlanAction::UseOverflowPool, plan.action);
	EXPECT_EQ(0, plan.next_preferred_slot);
}
