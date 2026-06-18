#include "core_host_context.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class MainThreadPump {
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<aegisub::core::CoreHostTask> jobs;
	std::thread::id main_thread_id = std::this_thread::get_id();

public:
	aegisub::core::CoreHostThreadContext MakeContext() {
		return aegisub::core::CoreHostThreadContext({
			[this](aegisub::core::CoreHostTask task) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					jobs.emplace_back(std::move(task));
				}
				cv.notify_all();
			},
			[this] {
				return std::this_thread::get_id() == main_thread_id;
			},
			[this] {
				return Flush();
			},
		});
	}

	void Notify() {
		cv.notify_all();
	}

	std::size_t Flush() {
		std::size_t ran = 0;
		while (true) {
			aegisub::core::CoreHostTask task;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (jobs.empty())
					return ran;

				task = std::move(jobs.front());
				jobs.pop_front();
			}
			task();
			++ran;
		}
	}

	template<typename Done>
	void RunUntil(Done&& done) {
		while (!done()) {
			if (Flush() != 0)
				continue;

			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [&] {
				return done() || !jobs.empty();
			});
		}

		while (Flush() != 0) {
		}
	}
};

struct BenchResult {
	std::string name;
	std::size_t iterations = 0;
	double ns_per_op = 0.0;
	std::uint64_t observed = 0;
};

template<typename Func>
BenchResult Measure(std::string name, std::size_t iterations, Func&& func) {
	auto const started = Clock::now();
	auto const observed = func();
	auto const elapsed_ns = std::chrono::duration<double, std::nano>(Clock::now() - started).count();
	return {
		std::move(name),
		iterations,
		elapsed_ns / static_cast<double>(iterations),
		observed,
	};
}

BenchResult BenchInlineInvoke(aegisub::core::CoreHostThreadContext const& context, std::size_t iterations) {
	std::uint64_t sink = 0;
	auto result = Measure("main_inline_invoke", iterations, [&] {
		for (std::size_t i = 0; i < iterations; ++i) {
			context.InvokeOnMain([&] {
				++sink;
			});
		}
		return sink;
	});
	if (result.observed != iterations)
		throw std::runtime_error("inline invoke benchmark did not execute every callback");
	return result;
}

BenchResult BenchPostAndFlush(MainThreadPump& pump, aegisub::core::CoreHostThreadContext const& context, std::size_t iterations) {
	std::uint64_t sink = 0;
	auto result = Measure("post_then_flush", iterations, [&] {
		for (std::size_t i = 0; i < iterations; ++i) {
			context.PostToMain([&] {
				++sink;
			});
		}
		auto const flushed = pump.Flush();
		if (flushed != iterations)
			throw std::runtime_error("post benchmark did not flush every callback");
		return sink;
	});
	if (result.observed != iterations)
		throw std::runtime_error("post benchmark did not execute every callback");
	return result;
}

BenchResult BenchBackgroundInvoke(MainThreadPump& pump, aegisub::core::CoreHostThreadContext const& context, std::size_t iterations) {
	std::atomic_uint64_t sink{0};
	std::atomic_bool done{false};

	auto const started = Clock::now();
	std::thread worker([&] {
		for (std::size_t i = 0; i < iterations; ++i) {
			context.InvokeOnMain([&] {
				sink.fetch_add(1, std::memory_order_relaxed);
			});
		}
		done.store(true, std::memory_order_release);
		pump.Notify();
	});

	pump.RunUntil([&] {
		return done.load(std::memory_order_acquire);
	});
	worker.join();

	auto const elapsed_ns = std::chrono::duration<double, std::nano>(Clock::now() - started).count();
	auto const observed = sink.load(std::memory_order_relaxed);
	if (observed != iterations)
		throw std::runtime_error("background invoke benchmark did not execute every callback");

	return {
		"background_sync_invoke",
		iterations,
		elapsed_ns / static_cast<double>(iterations),
		observed,
	};
}

BenchResult BenchBackgroundPost(MainThreadPump& pump, aegisub::core::CoreHostThreadContext const& context, std::size_t iterations) {
	std::atomic_uint64_t sink{0};
	std::atomic_bool posted{false};

	auto const started = Clock::now();
	std::thread worker([&] {
		for (std::size_t i = 0; i < iterations; ++i) {
			context.PostToMain([&] {
				sink.fetch_add(1, std::memory_order_relaxed);
			});
		}
		posted.store(true, std::memory_order_release);
		pump.Notify();
	});

	pump.RunUntil([&] {
		return posted.load(std::memory_order_acquire)
			&& sink.load(std::memory_order_relaxed) == iterations;
	});
	worker.join();

	auto const elapsed_ns = std::chrono::duration<double, std::nano>(Clock::now() - started).count();
	auto const observed = sink.load(std::memory_order_relaxed);
	if (observed != iterations)
		throw std::runtime_error("background post benchmark did not execute every callback");

	return {
		"background_async_post",
		iterations,
		elapsed_ns / static_cast<double>(iterations),
		observed,
	};
}

void PrintResults(std::vector<BenchResult> const& results) {
	std::cout << "Core host boundary bench\n";
	std::cout << std::left
	          << std::setw(26) << "scenario"
	          << std::right
	          << std::setw(14) << "iterations"
	          << std::setw(16) << "ns/op"
	          << std::setw(14) << "observed"
	          << "\n";

	for (auto const& result : results) {
		std::cout << std::left
		          << std::setw(26) << result.name
		          << std::right
		          << std::setw(14) << result.iterations
		          << std::setw(16) << std::fixed << std::setprecision(1) << result.ns_per_op
		          << std::setw(14) << result.observed
		          << "\n";
	}
}

} // namespace

int main() {
	try {
		MainThreadPump pump;
		auto context = pump.MakeContext();

		std::vector<BenchResult> results;
		results.push_back(BenchInlineInvoke(context, 1'000'000));
		results.push_back(BenchPostAndFlush(pump, context, 200'000));
		results.push_back(BenchBackgroundPost(pump, context, 100'000));
		results.push_back(BenchBackgroundInvoke(pump, context, 20'000));

		PrintResults(results);
		return 0;
	}
	catch (std::exception const& err) {
		std::cerr << "core-host-boundary-bench failed: " << err.what() << "\n";
		return 1;
	}
}
