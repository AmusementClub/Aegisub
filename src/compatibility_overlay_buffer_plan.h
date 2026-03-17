// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <array>

enum class CompatibilityOverlayBufferPlanAction {
	UseSlot0,
	UseSlot1,
	UseOverflowPool
};

struct CompatibilityOverlayBufferSlotState {
	bool allocated = false;
	bool reusable = false;
	bool holds_previous = false;
};

struct CompatibilityOverlayBufferPlan {
	CompatibilityOverlayBufferPlanAction action = CompatibilityOverlayBufferPlanAction::UseSlot0;
	int next_preferred_slot = 0;
};

inline CompatibilityOverlayBufferPlan DecideCompatibilityOverlayBufferPlan(
	int preferred_slot,
	std::array<CompatibilityOverlayBufferSlotState, 2> const& slots) {
	CompatibilityOverlayBufferPlan plan;
	int normalized_preferred = (preferred_slot & 1);

	for (int attempt = 0; attempt < 2; ++attempt) {
		int slot_index = (normalized_preferred + attempt) & 1;
		auto const& slot = slots[static_cast<size_t>(slot_index)];
		if (slot.holds_previous)
			continue;
		if (!slot.allocated || slot.reusable) {
			plan.action = slot_index == 0
				? CompatibilityOverlayBufferPlanAction::UseSlot0
				: CompatibilityOverlayBufferPlanAction::UseSlot1;
			plan.next_preferred_slot = slot_index ^ 1;
			return plan;
		}
	}

	plan.action = CompatibilityOverlayBufferPlanAction::UseOverflowPool;
	plan.next_preferred_slot = normalized_preferred;
	return plan;
}
