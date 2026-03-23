#include <main.h>

#include "../../src/ui_dispatch.h"

#include <libaegisub/signal.h>

#include <deque>
#include <mutex>
#include <thread>

namespace {
class UiDispatchFixture : public ::testing::Test {
protected:
	std::mutex mutex;
	std::deque<agi::dispatch::Thunk> main_queue;
	std::thread::id main_thread_id = std::this_thread::get_id();

	void SetUp() override {
		agi::dispatch::Init([this](agi::dispatch::Thunk thunk) {
			std::lock_guard<std::mutex> lock(mutex);
			main_queue.emplace_back(std::move(thunk));
		}, [this] {
			return std::this_thread::get_id() == main_thread_id;
		}, [this] {
			return PumpMainJobs();
		});
	}

	void TearDown() override {
		agi::dispatch::Init([](agi::dispatch::Thunk) { }, [] {
			return false;
		}, [] {
			return std::size_t{0};
		});
	}

	std::size_t PumpMainJobs() {
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

TEST_F(UiDispatchFixture, main_invoke_runs_inline_on_main_thread) {
	ASSERT_TRUE(agi::ui::CheckAccess());

	int calls = 0;
	auto result = agi::ui::MainInvoke([&] {
		++calls;
		return 42;
	});

	EXPECT_EQ(42, result);
	EXPECT_EQ(1, calls);
}

TEST_F(UiDispatchFixture, activation_scope_disconnects_signal_connections_on_deactivate) {
	agi::ui::UiActivationScope scope;
	agi::signal::Signal<> signal;
	int calls = 0;

	scope.AddConnection(signal.Connect([&] {
		++calls;
	}));

	signal();
	EXPECT_EQ(1, calls);

	scope.Deactivate();
	signal();
	EXPECT_EQ(1, calls);
}

TEST_F(UiDispatchFixture, deactivated_scope_drops_queued_main_callbacks) {
	agi::ui::UiActivationScope scope;
	int calls = 0;

	agi::ui::MainAsyncIfAlive(scope.GetLifetime(), [&] {
		++calls;
	});
	scope.Deactivate();

	EXPECT_EQ(1u, agi::dispatch::RunMainJobsForTests());
	EXPECT_EQ(0, calls);
}
