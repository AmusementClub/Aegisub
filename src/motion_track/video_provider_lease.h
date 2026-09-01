#pragma once

// The state object is owned by the Project via shared_ptr and outlives every
// provider it guards. Analyze tasks acquire a handle BEFORE starting their
// progress run; the handle pins the current generation so a later retirement
// is observable from the task side (ChangeRequested()) without ever touching
// the Project.
//
// Retirement protocol (all calls from the control thread):
//   Begin()    -> deny new leases, flag outstanding ones as changed
//   Drain()    -> block until every outstanding handle is released
//   Complete() -> allow new leases against the new generation
// Project swaps/destroys the provider only between Drain() and Complete().
// The destructor must run the same Begin()/Drain() sequence before its other
// members unwind.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace aegisub::motion_track {

class VideoProviderLeaseState
: public std::enable_shared_from_this<VideoProviderLeaseState> {
public:
	struct Token {
		explicit Token() = default;
	};

	explicit VideoProviderLeaseState(Token) {}

	static std::shared_ptr<VideoProviderLeaseState> Create() {
		return std::make_shared<VideoProviderLeaseState>(Token{});
	}

	class Handle {
	public:
		Handle() = default;
		Handle(Handle&& other) noexcept
		: state(std::move(other.state)), generation(other.generation) {
			other.generation = 0;
		}
		Handle& operator=(Handle&& other) noexcept {
			if (this != &other) {
				Release();
				state = std::move(other.state);
				generation = other.generation;
				other.generation = 0;
			}
			return *this;
		}
		~Handle() { Release(); }

		explicit operator bool() const noexcept { return state != nullptr; }

		/// Generation this handle was acquired against; 0 for an empty handle.
		/// Recorded in the published snapshot so a consumer can tell which
		/// provider a trajectory was measured against.
		std::uint64_t Generation() const noexcept { return generation; }

		/// True when a retirement began after this handle was acquired, i.e.
		/// the provider behind this lease is going away or already gone.
		bool ChangeRequested() const noexcept {
			if (!state) return true;
			return state->retiring.load(std::memory_order_acquire)
			    || generation !=
			           state->generation.load(std::memory_order_relaxed);
		}

		void Release() {
			if (!state) return;
			auto const saved = state;
			state.reset();
			std::lock_guard<std::mutex> lock(saved->mutex);
			if (saved->active_leases > 0 && --saved->active_leases == 0)
				saved->cv.notify_all();
		}

	private:
		friend class VideoProviderLeaseState;
		Handle(std::shared_ptr<VideoProviderLeaseState> state,
		       std::uint64_t generation)
		: state(std::move(state)), generation(generation) {}

		std::shared_ptr<VideoProviderLeaseState> state;
		std::uint64_t generation = 0;
	};

	/// Acquire a lease against the current generation. Fails while retiring.
	Handle Acquire() {
		std::unique_lock<std::mutex> lock(mutex);
		if (retiring.load(std::memory_order_relaxed))
			return {};
		++active_leases;
		return Handle(shared_from_this(),
		              generation.load(std::memory_order_relaxed));
	}
	/// Deny new leases and flag outstanding ones; never blocks.
	void Begin() {
		std::lock_guard<std::mutex> lock(mutex);
		retiring.store(true, std::memory_order_release);
		generation.fetch_add(1, std::memory_order_relaxed);
	}

	/// Allow new leases again (call after the provider has been replaced).
	void Complete() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			retiring.store(false, std::memory_order_release);
		}
		cv.notify_all();
	}

	/// Block until every outstanding handle has been released.
	void Drain() {
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [this] { return active_leases == 0; });
	}

	template <typename Rep, typename Period>
	bool DrainFor(std::chrono::duration<Rep, Period> timeout) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, timeout,
		    [this] { return active_leases == 0; });
	}

private:
	explicit VideoProviderLeaseState() = default;

	std::mutex mutex;
	std::condition_variable cv;
	std::atomic<bool> retiring{false};
	std::atomic<std::uint64_t> generation{1};
	int active_leases = 0;
};

} // namespace aegisub::motion_track
