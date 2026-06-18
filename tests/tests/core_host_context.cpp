#include <main.h>

#include "../../src/core_host_context.h"
#include "../../src/translation_service.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

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

	bool WaitForJobs(std::size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return jobs.size() >= count; });
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
};

class PrefixTranslationService final : public TranslationService {
public:
	std::string Translate(std::string const& msgid) const override {
		return "translated:" + msgid;
	}

	std::string TranslatePlural(std::string const& msgid,
	                            std::string const& msgid_plural,
	                            unsigned long n) const override {
		return "translated:" + (n == 1 ? msgid : msgid_plural);
	}
};

} // namespace

TEST(core_host_context, invoke_runs_inline_when_already_on_host_main_thread) {
	MainThreadPump pump;
	auto context = pump.MakeContext();

	int calls = 0;
	auto result = context.InvokeOnMain([&] {
		++calls;
		return 42;
	});

	EXPECT_EQ(42, result);
	EXPECT_EQ(1, calls);
	EXPECT_EQ(0u, pump.Flush());
}

TEST(core_host_context, background_thread_invokes_on_host_main_thread_and_returns_value) {
	MainThreadPump pump;
	auto context = pump.MakeContext();

	std::thread::id callback_thread;
	int result = 0;
	std::exception_ptr error;
	std::thread worker([&] {
		try {
			result = context.InvokeOnMain([&] {
				callback_thread = std::this_thread::get_id();
				EXPECT_TRUE(context.IsMainThread());
				return 7;
			});
		}
		catch (...) {
			error = std::current_exception();
		}
	});

	ASSERT_TRUE(pump.WaitForJobs(1));
	EXPECT_EQ(1u, context.FlushMainJobsForHost());
	worker.join();
	if (error)
		std::rethrow_exception(error);

	EXPECT_EQ(7, result);
	EXPECT_NE(std::thread::id(), callback_thread);
	EXPECT_TRUE(context.IsMainThread());
}

TEST(core_host_context, post_to_main_uses_host_queue_without_gui_framework_types) {
	MainThreadPump pump;
	auto context = pump.MakeContext();

	bool ran = false;
	std::thread worker([&] {
		context.PostToMain([&] {
			ran = context.IsMainThread();
		});
	});

	ASSERT_TRUE(pump.WaitForJobs(1));
	EXPECT_EQ(1u, context.FlushMainJobsForHost());
	worker.join();

	EXPECT_TRUE(ran);
}

TEST(core_host_context, invoke_propagates_exceptions_to_background_caller) {
	MainThreadPump pump;
	auto context = pump.MakeContext();

	std::exception_ptr error;
	std::thread worker([&] {
		try {
			context.InvokeOnMain([] {
				throw std::runtime_error("host-main failure");
			});
		}
		catch (...) {
			error = std::current_exception();
		}
	});

	ASSERT_TRUE(pump.WaitForJobs(1));
	EXPECT_EQ(1u, context.FlushMainJobsForHost());
	worker.join();

	ASSERT_TRUE(static_cast<bool>(error));
	EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
}

TEST(core_host_context, invoke_times_out_when_host_accepts_but_does_not_run_task) {
	auto context = aegisub::core::CoreHostThreadContext({
		[](aegisub::core::CoreHostTask) {
		},
		[] {
			return false;
		},
		{},
		std::chrono::milliseconds(5),
	});

	EXPECT_THROW(context.InvokeOnMain([] {
		return 1;
	}), aegisub::core::CoreHostTimeoutError);
}

TEST(core_host_context, invoke_timeout_abandons_late_host_task) {
	aegisub::core::CoreHostTask queued_task;
	auto context = aegisub::core::CoreHostThreadContext({
		[&](aegisub::core::CoreHostTask task) {
			queued_task = std::move(task);
		},
		[] {
			return false;
		},
		{},
		std::chrono::milliseconds(5),
	});

	bool ran = false;
	EXPECT_THROW(context.InvokeOnMain([&] {
		ran = true;
		return 1;
	}), aegisub::core::CoreHostTimeoutError);

	ASSERT_TRUE(static_cast<bool>(queued_task));
	queued_task();
	EXPECT_FALSE(ran);
}

TEST(translation_context, default_service_is_visible_to_new_threads) {
	PrefixTranslationService service;
	TranslationContext::Reset();
	TranslationContext::SetDefault(&service);

	std::string translated;
	std::thread worker([&] {
		translated = TranslationContext::Get().Translate("hello");
	});
	worker.join();

	TranslationContext::Reset();
	TranslationContext::ResetDefault();
	EXPECT_EQ("translated:hello", translated);
}
