#pragma once

#include <atomic>
#include <memory>

// The worker acquires slots; XAudio2 callbacks release them. A second bank lets
// a seek submit new audio while the flushed bank awaits its OnBufferEnd calls.
class XAudio2BufferSlots final {
	int buffers_per_bank;
	int active_begin = 0;
	std::unique_ptr<std::atomic_bool[]> occupied;

	public:
	explicit XAudio2BufferSlots(int buffers_per_bank)
		: buffers_per_bank(buffers_per_bank), occupied(std::make_unique<std::atomic_bool[]>(2 * buffers_per_bank)) {
		for (int i = 0; i < 2 * buffers_per_bank; ++i)
			occupied[i].store(false, std::memory_order_relaxed);
	}

	[[nodiscard]] int SlotCount() const noexcept { return 2 * buffers_per_bank; }

	void BeginPlayback() noexcept {
		active_begin = active_begin == 0 ? buffers_per_bank : 0;
	}

	// Returns -1 when all slots in this playback bank still belong to XAudio2.
	int TryAcquire() noexcept {
		for (int i = active_begin; i < active_begin + buffers_per_bank; ++i) {
			if (!occupied[i].exchange(true, std::memory_order_acq_rel)) {
				return i;
			}
		}
		return -1;
	}

	void Release(int slot) noexcept {
		if (slot >= 0 && slot < SlotCount()) {
			occupied[slot].store(false, std::memory_order_release);
		}
	}
};
