#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

class ReadyFlag {
	mutable std::mutex mutex;
	std::condition_variable cv;
	bool ready = false;

public:
	bool IsReady() const {
		std::lock_guard<std::mutex> lock(mutex);
		return ready;
	}

	void Signal() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			ready = true;
		}
		cv.notify_all();
	}

	void Reset() {
		std::lock_guard<std::mutex> lock(mutex);
		ready = false;
	}

	template<typename Rep, typename Period>
	bool WaitFor(std::chrono::duration<Rep, Period> const& timeout) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, timeout, [&] { return ready; });
	}

	void Wait() {
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&] { return ready; });
	}
};
