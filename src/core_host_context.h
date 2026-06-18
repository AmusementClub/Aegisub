#pragma once

#include <libaegisub/dispatch.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aegisub::core {

using CoreHostTask = agi::dispatch::Thunk;

struct CoreHostThreadHooks {
	std::function<void(CoreHostTask)> post_to_main;
	std::function<bool()> is_main_thread;
	std::function<std::size_t()> flush_main_jobs;
	std::chrono::milliseconds synchronous_invoke_timeout = std::chrono::seconds(30);
};

// WARNING: CoreHostThreadContext::InvokeOnMain blocks the calling thread on a
// condition variable until the host main thread runs the marshaled callback
// (default timeout 30s, then CoreHostTimeoutError). It is therefore unsafe on
// hot paths and from any code path the host main thread may be synchronously
// waiting on, or it will deadlock until the timeout fires.
//
// The structural test
//   host_thread_and_registry_lifecycle_boundaries_stay_out_of_media_hot_paths
// (tests/tests/host_boundary_policy.cpp) forbids core_host_context.h,
// InvokeOnMain(, PostToMain(, and provider_factory_registry.h from appearing
// in media hot paths (async_video_provider, renderers, concrete providers).
// When adding new call sites, prefer PostToMain (fire-and-forget) or have the
// host own the lifetime; only use InvokeOnMain for rare startup/shutdown or
// user-initiated operations where blocking the caller is acceptable.
class CoreHostThreadContext {
	CoreHostThreadHooks hooks;

	void WaitForMainInvocation(std::unique_lock<std::mutex>& lock,
	                           std::condition_variable& cv,
	                           bool& done,
	                           bool& abandoned) const;

public:
	CoreHostThreadContext();
	explicit CoreHostThreadContext(CoreHostThreadHooks hooks);

	static CoreHostThreadContext FromGlobalDispatch();

	bool IsMainThread() const;
	void PostToMain(CoreHostTask task) const;
	std::size_t FlushMainJobsForHost() const;

	template<typename Callback>
	auto InvokeOnMain(Callback&& callback) const -> std::invoke_result_t<std::decay_t<Callback>&> {
		using CallbackType = std::decay_t<Callback>;
		using Result = std::invoke_result_t<CallbackType&>;

		if (IsMainThread()) {
			if constexpr (std::is_void_v<Result>) {
				std::forward<Callback>(callback)();
				return;
			}
			else {
				return std::forward<Callback>(callback)();
			}
		}

		struct InvocationState {
			std::mutex mutex;
			std::condition_variable cv;
			std::exception_ptr error;
			bool done = false;
			bool abandoned = false;
		};

		auto callback_ptr = std::make_shared<CallbackType>(std::forward<Callback>(callback));

		if constexpr (std::is_void_v<Result>) {
			auto state = std::make_shared<InvocationState>();
			PostToMain([callback_ptr = std::move(callback_ptr), state] {
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (state->abandoned)
						return;
				}

				std::exception_ptr error;
				try {
					(*callback_ptr)();
				}
				catch (...) {
					error = std::current_exception();
				}

				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (state->abandoned)
						return;
					state->error = error;
					state->done = true;
				}
				state->cv.notify_one();
			});

			std::unique_lock<std::mutex> lock(state->mutex);
			WaitForMainInvocation(lock, state->cv, state->done, state->abandoned);
			if (state->error)
				std::rethrow_exception(state->error);
			return;
		}
		else {
			using Storage = std::conditional_t<
				std::is_reference_v<Result>,
				std::reference_wrapper<std::remove_reference_t<Result>>,
				std::remove_cv_t<Result>>;
			struct ValueInvocationState : InvocationState {
				std::optional<Storage> result;
			};
			auto value_state = std::make_shared<ValueInvocationState>();

			PostToMain([callback_ptr = std::move(callback_ptr), value_state] {
				{
					std::lock_guard<std::mutex> lock(value_state->mutex);
					if (value_state->abandoned)
						return;
				}

				std::exception_ptr error;
				std::optional<Storage> result;
				try {
					result.emplace((*callback_ptr)());
				}
				catch (...) {
					error = std::current_exception();
				}

				{
					std::lock_guard<std::mutex> lock(value_state->mutex);
					if (value_state->abandoned)
						return;
					value_state->error = error;
					if (result)
						value_state->result.emplace(std::move(*result));
					value_state->done = true;
				}
				value_state->cv.notify_one();
			});

			std::unique_lock<std::mutex> lock(value_state->mutex);
			WaitForMainInvocation(lock, value_state->cv, value_state->done, value_state->abandoned);
			if (value_state->error)
				std::rethrow_exception(value_state->error);

			if constexpr (std::is_reference_v<Result>)
				return value_state->result->get();
			else
				return std::move(*value_state->result);
		}
	}
};

class CoreHostTimeoutError final : public std::runtime_error {
public:
	CoreHostTimeoutError();
};

} // namespace aegisub::core
