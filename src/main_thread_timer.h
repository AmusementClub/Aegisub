#pragma once

#include <libaegisub/dispatch.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

class MainThreadTimer {
	struct State final : std::enable_shared_from_this<State> {
		mutable std::mutex mutex;
		std::condition_variable cv;
		std::function<void()> callback;
		std::jthread worker;
		std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now();
		int interval_ms = 0;
		std::uint64_t generation = 0;
		bool armed = false;
		bool repeating = false;
		bool shutting_down = false;

		explicit State(std::function<void()> callback)
		: callback(std::move(callback)) {
		}

		void StartWorker() {
			worker = std::jthread([this] { WorkerLoop(); });
		}

		void WorkerLoop() {
			std::unique_lock<std::mutex> lock(mutex);
			while (true) {
				cv.wait(lock, [&] { return shutting_down || armed; });
				if (shutting_down)
					return;

				auto const generation_snapshot = generation;
				auto const deadline_snapshot = deadline;
				cv.wait_until(lock, deadline_snapshot, [&] {
					return shutting_down || !armed || generation != generation_snapshot;
				});
				if (shutting_down)
					return;
				if (!armed || generation != generation_snapshot)
					continue;
				if (std::chrono::steady_clock::now() < deadline_snapshot)
					continue;

				if (repeating)
					deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);
				else
					armed = false;

				auto weak_state = weak_from_this();
				lock.unlock();
				agi::dispatch::Main().Async([weak_state, generation_snapshot] {
					if (auto state = weak_state.lock())
						state->RunIfCurrent(generation_snapshot);
				});
				lock.lock();
			}
		}

		void RunIfCurrent(std::uint64_t expected_generation) {
			std::function<void()> current_callback;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (shutting_down || generation != expected_generation)
					return;
				current_callback = callback;
			}

			if (current_callback)
				current_callback();
		}

		void Start(int delay_ms, bool repeat) {
			std::lock_guard<std::mutex> lock(mutex);
			++generation;
			interval_ms = delay_ms < 0 ? 0 : delay_ms;
			repeating = repeat;
			armed = true;
			deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);
			cv.notify_all();
		}

		void Stop() {
			std::lock_guard<std::mutex> lock(mutex);
			++generation;
			armed = false;
			repeating = false;
			cv.notify_all();
		}

		bool IsRunning() const {
			std::lock_guard<std::mutex> lock(mutex);
			return armed;
		}

		void Shutdown() {
			{
				std::lock_guard<std::mutex> lock(mutex);
				++generation;
				armed = false;
				repeating = false;
				shutting_down = true;
			}
			cv.notify_all();
			if (worker.joinable())
				worker.join();
		}

		~State() = default;
	};

	std::shared_ptr<State> state;

public:
	explicit MainThreadTimer(std::function<void()> callback)
	: state(std::make_shared<State>(std::move(callback))) {
		state->StartWorker();
	}

	~MainThreadTimer() {
		if (state)
			state->Shutdown();
	}

	MainThreadTimer(MainThreadTimer const&) = delete;
	MainThreadTimer& operator=(MainThreadTimer const&) = delete;
	MainThreadTimer(MainThreadTimer&&) = delete;
	MainThreadTimer& operator=(MainThreadTimer&&) = delete;

	void StartOnce(int delay_ms) {
		state->Start(delay_ms, false);
	}

	void StartRepeating(int interval_ms) {
		state->Start(interval_ms, true);
	}

	void Stop() {
		state->Stop();
	}

	bool IsRunning() const {
		return state->IsRunning();
	}
};
