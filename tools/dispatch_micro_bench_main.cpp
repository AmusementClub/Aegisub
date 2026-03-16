#include <libaegisub/dispatch.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

struct BenchResult {
	std::string name;
	double ns_per_op = 0.0;
};

template<typename Func>
BenchResult run_bench(char const *name, std::size_t iterations, Func&& func) {
	auto start = clock_type::now();
	func(iterations);
	auto end = clock_type::now();
	double total_ns = std::chrono::duration<double, std::nano>(end - start).count();
	return {name, total_ns / iterations};
}

void print_results(std::vector<BenchResult> const& results) {
	std::cout << std::left << std::setw(28) << "name"
		<< std::right << std::setw(14) << "ns/op"
		<< "\n";
	for (auto const& result : results) {
		std::cout << std::left << std::setw(28) << result.name
			<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << result.ns_per_op
			<< "\n";
	}
}

void background_roundtrip(std::size_t iterations) {
	std::mutex mutex;
	std::condition_variable cv;
	std::size_t remaining = iterations;

	for (std::size_t i = 0; i < iterations; ++i) {
		agi::dispatch::Background().Async([&] {
			std::lock_guard<std::mutex> lock(mutex);
			if (--remaining == 0)
				cv.notify_one();
		});
	}

	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] { return remaining == 0; });
}

void background_multi_producer(std::size_t iterations) {
	std::mutex mutex;
	std::condition_variable cv;
	std::size_t remaining = iterations;
	constexpr std::size_t producers = 4;

	std::vector<std::thread> workers;
	workers.reserve(producers);
	for (std::size_t producer = 0; producer < producers; ++producer) {
		workers.emplace_back([&, producer] {
			for (std::size_t i = producer; i < iterations; i += producers) {
				agi::dispatch::Background().Async([&] {
					std::lock_guard<std::mutex> lock(mutex);
					if (--remaining == 0)
						cv.notify_one();
				});
			}
		});
	}
	for (auto& worker : workers)
		worker.join();

	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] { return remaining == 0; });
}

void serial_post_flush(std::size_t iterations) {
	auto serial = agi::dispatch::Create();
	std::atomic<std::size_t> sum = 0;
	for (std::size_t i = 0; i < iterations; ++i)
		serial->Async([&] { ++sum; });
	serial->Sync([&] { (void)sum.load(); });
}

void serial_chain_enqueue(std::size_t iterations) {
	auto serial = agi::dispatch::Create();
	std::mutex mutex;
	std::condition_variable cv;
	std::atomic<std::size_t> completed = 0;
	auto chain = std::make_shared<std::function<void()>>();
	*chain = [&, chain] {
		if (completed.fetch_add(1, std::memory_order_relaxed) + 1 == iterations) {
			std::lock_guard<std::mutex> lock(mutex);
			cv.notify_one();
			return;
		}
		serial->Async(*chain);
	};
	serial->Async(*chain);

	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] { return completed.load(std::memory_order_relaxed) == iterations; });
}

void serial_sync_small(std::size_t iterations) {
	auto serial = agi::dispatch::Create();
	for (std::size_t i = 0; i < iterations; ++i)
		serial->Sync([] { });
}

void main_post_inline(std::size_t iterations) {
	std::atomic<std::size_t> sum = 0;
	for (std::size_t i = 0; i < iterations; ++i)
		agi::dispatch::Main().Async([&] { ++sum; });
}
}

int main() {
	agi::dispatch::Init([](agi::dispatch::Thunk thunk) { thunk(); });

	std::cout << "Dispatch micro benchmark\n";
	print_results({
		run_bench("background_post_roundtrip", 20000, background_roundtrip),
		run_bench("background_multi_producer", 20000, background_multi_producer),
		run_bench("serial_post_flush", 20000, serial_post_flush),
		run_bench("serial_chain_enqueue", 20000, serial_chain_enqueue),
		run_bench("serial_sync_small", 10000, serial_sync_small),
		run_bench("main_post_wrapper", 200000, main_post_inline),
	});
	return 0;
}
