#include "audio_latest_range_scheduler.h"

#include <condition_variable>
#include <mutex>
#include <thread>

class AudioLatestRangeScheduler::Impl {
public:
	mutable std::mutex mutex;
	std::condition_variable_any cv;
	Callback callback;
	std::jthread worker;
	bool has_request = false;
	size_t first = 0;
	size_t last = 0;
	uint64_t generation = 0;

	explicit Impl(Callback callback)
	: callback(std::move(callback))
	, worker([this](std::stop_token stop_token) { WorkerLoop(stop_token); }) {
	}

	void WorkerLoop(std::stop_token stop_token) {
		while (!stop_token.stop_requested()) {
			size_t request_first = 0;
			size_t request_last = 0;
			uint64_t request_generation = 0;
			{
				std::unique_lock<std::mutex> lock(mutex);
				cv.wait(lock, stop_token, [&] { return has_request; });
				if (stop_token.stop_requested())
					return;
				request_first = first;
				request_last = last;
				request_generation = generation;
				has_request = false;
			}

			if (callback)
				callback(request_first, request_last, request_generation);
		}
	}
};

AudioLatestRangeScheduler::AudioLatestRangeScheduler(Callback callback)
: impl(std::make_unique<Impl>(std::move(callback))) {
}

AudioLatestRangeScheduler::~AudioLatestRangeScheduler() = default;

void AudioLatestRangeScheduler::Request(size_t first, size_t last) {
	if (last < first)
		return;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->first = first;
		impl->last = last;
		impl->has_request = true;
		++impl->generation;
	}
	impl->cv.notify_one();
}

void AudioLatestRangeScheduler::Invalidate() {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->has_request = false;
	++impl->generation;
}

uint64_t AudioLatestRangeScheduler::CurrentGeneration() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->generation;
}

bool AudioLatestRangeScheduler::IsCurrent(uint64_t request_generation) const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return request_generation == impl->generation;
}
