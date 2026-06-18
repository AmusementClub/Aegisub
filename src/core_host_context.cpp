#include "core_host_context.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace aegisub::core {

namespace {

CoreHostThreadHooks DefaultHooks() {
	return {
		[](CoreHostTask task) {
			agi::dispatch::Main().Async(std::move(task));
		},
		[] {
			return agi::dispatch::IsMainThread();
		},
		[] {
			return agi::dispatch::RunMainJobsForTests();
		},
	};
}

} // namespace

CoreHostTimeoutError::CoreHostTimeoutError()
: std::runtime_error("timed out waiting for host main-thread invocation") {
}

CoreHostThreadContext::CoreHostThreadContext()
: hooks(DefaultHooks()) {
}

CoreHostThreadContext::CoreHostThreadContext(CoreHostThreadHooks hooks)
: hooks(std::move(hooks)) {
}

CoreHostThreadContext CoreHostThreadContext::FromGlobalDispatch() {
	return CoreHostThreadContext(DefaultHooks());
}

bool CoreHostThreadContext::IsMainThread() const {
	return hooks.is_main_thread && hooks.is_main_thread();
}

void CoreHostThreadContext::PostToMain(CoreHostTask task) const {
	if (!hooks.post_to_main)
		throw std::logic_error("core host thread context has no main-thread post hook");
	hooks.post_to_main(std::move(task));
}

std::size_t CoreHostThreadContext::FlushMainJobsForHost() const {
	return hooks.flush_main_jobs ? hooks.flush_main_jobs() : std::size_t{0};
}

void CoreHostThreadContext::WaitForMainInvocation(std::unique_lock<std::mutex>& lock,
                                                  std::condition_variable& cv,
                                                  bool& done,
                                                  bool& abandoned) const {
	if (hooks.synchronous_invoke_timeout <= std::chrono::milliseconds::zero()) {
		cv.wait(lock, [&] { return done; });
		return;
	}

	if (!cv.wait_for(lock, hooks.synchronous_invoke_timeout, [&] { return done; })) {
		abandoned = true;
		throw CoreHostTimeoutError();
	}
}

} // namespace aegisub::core
