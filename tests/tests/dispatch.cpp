#include <main.h>

#include <libaegisub/dispatch.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
class DispatchFixture : public ::testing::Test {
protected:
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<agi::dispatch::Thunk> main_queue;
	std::thread::id main_thread_id = std::this_thread::get_id();

	void SetUp() override {
		agi::dispatch::Init([this](agi::dispatch::Thunk thunk) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				main_queue.emplace_back(std::move(thunk));
			}
			cv.notify_all();
		}, [this] {
			return std::this_thread::get_id() == main_thread_id;
		}, [this] {
			return PumpMainTasks();
		});
	}

	void TearDown() override {
		agi::dispatch::Init([](agi::dispatch::Thunk) { }, [] {
			return false;
		}, [] {
			return std::size_t{0};
		});
	}

	bool WaitForMainTasks(size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return main_queue.size() >= count; });
	}

	std::size_t PumpMainTasks() {
		std::size_t executed = 0;
		while (true) {
			agi::dispatch::Thunk thunk;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (main_queue.empty())
					return executed;

				thunk = std::move(main_queue.front());
				main_queue.pop_front();
			}
			++executed;
			thunk();
		}
	}
};
}

TEST_F(DispatchFixture, background_executor_runs_tasks) {
	std::mutex done_mutex;
	std::condition_variable done_cv;
	bool done = false;

	agi::dispatch::BackgroundExecutor().Post([&] {
		{
			std::lock_guard<std::mutex> lock(done_mutex);
			done = true;
		}
		done_cv.notify_one();
	});

	std::unique_lock<std::mutex> lock(done_mutex);
	ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; }));
}

TEST_F(DispatchFixture, serial_queue_preserves_order) {
	auto queue = agi::dispatch::Create();
	std::mutex done_mutex;
	std::condition_variable done_cv;
	std::vector<int> values;

	for (int i = 0; i < 6; ++i) {
		queue->Async([&, i] {
			values.push_back(i);
			if (i == 5)
				done_cv.notify_one();
		});
	}

	std::unique_lock<std::mutex> lock(done_mutex);
	ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(2), [&] { return values.size() == 6; }));
	EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 5}), values);
}

TEST_F(DispatchFixture, sync_propagates_exceptions) {
	auto queue = agi::dispatch::Create();
	EXPECT_THROW(queue->Sync([] { throw std::runtime_error("boom"); }), std::runtime_error);
}

TEST_F(DispatchFixture, background_executor_does_not_drop_tasks) {
	std::mutex done_mutex;
	std::condition_variable done_cv;
	int completed = 0;

	for (int i = 0; i < 64; ++i) {
		agi::dispatch::Background().Async([&] {
			std::lock_guard<std::mutex> lock(done_mutex);
			++completed;
			if (completed == 64)
				done_cv.notify_one();
		});
	}

	std::unique_lock<std::mutex> lock(done_mutex);
	ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(2), [&] { return completed == 64; }));
}

TEST_F(DispatchFixture, async_exception_is_bridged_to_main_executor) {
	agi::dispatch::Background().Async([] { throw std::runtime_error("bridge"); });

	ASSERT_TRUE(WaitForMainTasks(1));
	EXPECT_THROW(agi::dispatch::RunMainJobsForTests(), std::runtime_error);
}

TEST_F(DispatchFixture, run_main_jobs_for_tests_drains_pending_queue) {
	std::vector<int> values;

	agi::dispatch::Main().Async([&] { values.push_back(1); });
	agi::dispatch::Main().Async([&] { values.push_back(2); });

	ASSERT_TRUE(WaitForMainTasks(2));
	EXPECT_EQ(2u, agi::dispatch::RunMainJobsForTests());
	EXPECT_EQ((std::vector<int>{1, 2}), values);
	EXPECT_EQ(0u, agi::dispatch::RunMainJobsForTests());
}

TEST_F(DispatchFixture, is_main_thread_uses_registered_checker) {
	EXPECT_TRUE(agi::dispatch::IsMainThread());

	std::mutex done_mutex;
	std::condition_variable done_cv;
	bool is_main = true;
	bool done = false;

	agi::dispatch::Background().Async([&] {
		{
			std::lock_guard<std::mutex> lock(done_mutex);
			is_main = agi::dispatch::IsMainThread();
			done = true;
		}
		done_cv.notify_one();
	});

	std::unique_lock<std::mutex> lock(done_mutex);
	ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; }));
	EXPECT_FALSE(is_main);
}
