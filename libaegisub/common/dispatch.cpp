// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#ifdef _WIN32
// For Boost::asio
#include <sdkddkver.h>
#endif

#include "libaegisub/dispatch.h"

#include "libaegisub/util.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
	std::function<void (agi::dispatch::Thunk)> invoke_main;
	std::function<bool ()> is_main_thread;
	std::function<std::size_t ()> flush_main_jobs;

	class ThreadPool {
		std::mutex mutex;
		std::condition_variable_any cv;
		std::deque<agi::dispatch::Thunk> tasks;
		std::vector<std::jthread> threads;

		void WorkerLoop(std::stop_token stop_token) {
			agi::util::SetThreadName("Dispatch Worker");
			while (true) {
				agi::dispatch::Thunk thunk;
				{
					std::unique_lock<std::mutex> lock(mutex);
					cv.wait(lock, stop_token, [&] { return !tasks.empty(); });
					if (stop_token.stop_requested())
						return;
					thunk = std::move(tasks.front());
					tasks.pop_front();
				}

				thunk();
			}
		}

	public:
		ThreadPool() {
			auto const worker_count = std::max<unsigned>(4, std::thread::hardware_concurrency());
			threads.reserve(worker_count);
			for (unsigned i = 0; i < worker_count; ++i)
				threads.emplace_back([this](std::stop_token stop_token) { WorkerLoop(stop_token); });
		}

		void Post(agi::dispatch::Thunk thunk) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				tasks.emplace_back(std::move(thunk));
			}
			cv.notify_one();
		}
	};

	ThreadPool& BackgroundPool() {
		static ThreadPool pool;
		return pool;
	}

	class MainQueue final : public agi::dispatch::Queue {
		void DoPost(agi::dispatch::Thunk thunk) override {
			invoke_main(std::move(thunk));
		}
	};

	class BackgroundQueue final : public agi::dispatch::Queue {
		void DoPost(agi::dispatch::Thunk thunk) override {
			BackgroundPool().Post(std::move(thunk));
		}
	};

	class SerialQueue final : public agi::dispatch::Queue {
		struct State {
			std::mutex mutex;
			std::deque<agi::dispatch::Thunk> tasks;
			bool drain_scheduled = false;

			void Drain() {
				agi::util::SetThreadName("Dispatch Serial");
				while (true) {
					agi::dispatch::Thunk thunk;
					{
						std::lock_guard<std::mutex> lock(mutex);
						if (tasks.empty()) {
							drain_scheduled = false;
							return;
						}
						thunk = std::move(tasks.front());
						tasks.pop_front();
					}

					thunk();
				}
			}
		};

		std::shared_ptr<State> state = std::make_shared<State>();

	public:
		void DoPost(agi::dispatch::Thunk thunk) override {
			bool should_schedule = false;
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->tasks.emplace_back(std::move(thunk));
				if (!state->drain_scheduled) {
					state->drain_scheduled = true;
					should_schedule = true;
				}
			}

			if (should_schedule)
				BackgroundPool().Post([state = state] { state->Drain(); });
		}
	};
}

namespace agi { namespace dispatch {

void Init(
	std::function<void (Thunk)> invoke_main,
	std::function<bool ()> is_main_thread,
	std::function<std::size_t ()> flush_main_jobs) {
	::invoke_main = std::move(invoke_main);
	::is_main_thread = is_main_thread
		? std::move(is_main_thread)
		: [] { return false; };
	::flush_main_jobs = flush_main_jobs
		? std::move(flush_main_jobs)
		: [] { return std::size_t{0}; };
	(void)BackgroundPool();
}

bool IsMainThread() {
	return is_main_thread && is_main_thread();
}

std::size_t RunMainJobsForTests() {
	return flush_main_jobs ? flush_main_jobs() : 0;
}

void Executor::Post(Thunk thunk) {
	DoPost([thunk = std::move(thunk)]() mutable {
		try {
			thunk();
		}
		catch (...) {
			auto e = std::current_exception();
			invoke_main([e] { std::rethrow_exception(e); });
		}
	});
}

void Executor::DoDispatch(Thunk thunk) {
	std::mutex m;
	std::condition_variable cv;
	std::unique_lock<std::mutex> lock(m);
	std::exception_ptr e;
	bool done = false;
	DoPost([&] {
		std::unique_lock<std::mutex> thunk_lock(m);
		try {
			thunk();
		}
		catch (...) {
			e = std::current_exception();
		}
		done = true;
		cv.notify_all();
	});
	cv.wait(lock, [&] { return done; });
	if (e) std::rethrow_exception(e);
}

void Executor::Dispatch(Thunk thunk) {
	DoDispatch(std::move(thunk));
}

void Queue::Async(Thunk thunk) {
	Post(std::move(thunk));
}

void Queue::Sync(Thunk thunk) {
	Dispatch(std::move(thunk));
}

Executor& MainExecutor() {
	return Main();
}

Executor& BackgroundExecutor() {
	return Background();
}

Queue& Main() {
	static MainQueue q;
	return q;
}

Queue& Background() {
	static BackgroundQueue q;
	return q;
}

std::unique_ptr<Executor> CreateExecutor() {
	return std::unique_ptr<Executor>(Create().release());
}

std::unique_ptr<Queue> Create() {
	return std::unique_ptr<Queue>(new SerialQueue);
}

} }
