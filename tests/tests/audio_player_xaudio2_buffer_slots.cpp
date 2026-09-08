#include <main.h>

#include "../../src/audio_player_xaudio2_buffer_slots.h"

#include <array>

TEST(xaudio2_buffer_slots, restart_acquires_without_waiting_for_previous_callbacks) {
	XAudio2BufferSlots slots(3);
	EXPECT_EQ(6, slots.SlotCount());
	slots.BeginPlayback();
	std::array<int, 3> old_slots;
	for (auto& slot : old_slots) {
		slot = slots.TryAcquire();
		ASSERT_GE(slot, 0);
	}
	EXPECT_EQ(-1, slots.TryAcquire());

	// No old callback has arrived. A new playback still has a complete bank.
	slots.BeginPlayback();
	std::array<int, 3> new_slots;
	for (auto& slot : new_slots) {
		slot = slots.TryAcquire();
		ASSERT_GE(slot, 0);
		for (auto old : old_slots) {
			EXPECT_NE(old, slot);
		}
	}
	EXPECT_NE(new_slots[0], new_slots[1]);
	EXPECT_NE(new_slots[0], new_slots[2]);
	EXPECT_NE(new_slots[1], new_slots[2]);
	EXPECT_EQ(-1, slots.TryAcquire());
}

TEST(xaudio2_buffer_slots, late_completion_does_not_release_current_buffers) {
	XAudio2BufferSlots slots(2);
	slots.BeginPlayback();
	int const old_first = slots.TryAcquire();
	int const old_second = slots.TryAcquire();
	ASSERT_GE(old_first, 0);
	ASSERT_GE(old_second, 0);
	slots.BeginPlayback();
	int const current_first = slots.TryAcquire();
	int const current_second = slots.TryAcquire();
	ASSERT_GE(current_first, 0);
	ASSERT_GE(current_second, 0);

	slots.Release(old_second);
	EXPECT_EQ(-1, slots.TryAcquire());
	slots.Release(current_first);
	EXPECT_EQ(current_first, slots.TryAcquire());
	slots.Release(old_first);
	EXPECT_EQ(-1, slots.TryAcquire());
	slots.Release(current_second);
	EXPECT_EQ(current_second, slots.TryAcquire());
	EXPECT_EQ(-1, slots.TryAcquire());
}

TEST(xaudio2_buffer_slots, rapid_restart_waits_for_each_slot_owner) {
	XAudio2BufferSlots slots(2);
	slots.BeginPlayback();
	int const first = slots.TryAcquire();
	int const second = slots.TryAcquire();
	ASSERT_GE(first, 0);
	ASSERT_GE(second, 0);
	slots.BeginPlayback();
	ASSERT_GE(slots.TryAcquire(), 0);
	ASSERT_GE(slots.TryAcquire(), 0);

	// Returning to a bank must not reset ownership of its still-live buffers.
	slots.BeginPlayback();
	EXPECT_EQ(-1, slots.TryAcquire());
	slots.Release(second);
	EXPECT_EQ(second, slots.TryAcquire());
	EXPECT_EQ(-1, slots.TryAcquire());
	slots.Release(first);
	EXPECT_EQ(first, slots.TryAcquire());
	EXPECT_EQ(-1, slots.TryAcquire());
}
