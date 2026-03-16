#include <main.h>

#include "../../src/ready_flag.h"

#include <atomic>
#include <chrono>
#include <thread>

TEST(ready_flag, wait_for_observes_signal) {
	ReadyFlag flag;
	std::jthread signaler([&](std::stop_token) {
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		flag.Signal();
	});

	EXPECT_TRUE(flag.WaitFor(std::chrono::milliseconds(250)));
}

TEST(ready_flag, wait_blocks_until_signal) {
	ReadyFlag flag;
	std::atomic<bool> completed = false;

	std::jthread waiter([&](std::stop_token) {
		flag.Wait();
		completed = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	EXPECT_FALSE(completed.load());
	flag.Signal();
	waiter.join();
	EXPECT_TRUE(completed.load());
}
