// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "headless_runtime_bootstrap.h"

#include "app_runtime.h"
#include "headless_cli.h"
#include "headless_playback_probe.h"

#include <libaegisub/dispatch.h>

#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace {

void ReportHeadlessError(std::string const& title, std::string const& message) {
	if (message.empty())
		return;
	if (!title.empty())
		std::cerr << "[" << title << "] ";
	std::cerr << message << std::endl;
}

class HeadlessMainThreadPump {
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<agi::dispatch::Thunk> jobs;
	std::thread::id main_thread_id = std::this_thread::get_id();

public:
	void Post(agi::dispatch::Thunk thunk) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			jobs.emplace_back(std::move(thunk));
		}
		cv.notify_all();
	}

	bool IsMainThread() const {
		return std::this_thread::get_id() == main_thread_id;
	}

	void Notify() {
		cv.notify_all();
	}

	std::size_t Flush() {
		std::size_t ran = 0;
		while (true) {
			agi::dispatch::Thunk thunk;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (jobs.empty())
					break;
				thunk = std::move(jobs.front());
				jobs.pop_front();
			}
			thunk();
			++ran;
		}
		return ran;
	}

	template<typename Predicate>
	void RunUntil(Predicate&& done) {
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

class HeadlessRuntimeEnvironment {
	HeadlessMainThreadPump main_thread_pump;
	AppRuntime runtime;

public:
	bool Initialize(std::string& error) {
		try {
			AppRuntimeInitOptions options;
			options.shell_mode = RuntimeShellMode::Headless;
			options.locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
			options.main_queue_hooks = {
				[this](agi::dispatch::Thunk thunk) {
					main_thread_pump.Post(std::move(thunk));
				},
				[this] {
					return main_thread_pump.IsMainThread();
				},
				[this] {
					return main_thread_pump.Flush();
				}
			};
			options.load_global_scripts = false;
			options.initialize_commands = false;
			options.initialize_ui_locale = false;
			options.register_automation_script_factory = false;
			options.warm_subtitles_provider_font_cache = false;
			options.register_export_filters = false;
			options.install_png_handler = false;
			options.report_nonfatal_error = [](std::string const& title, std::string const& message) {
				ReportHeadlessError(title, message);
			};
			return runtime.Initialize(std::move(options), error);
		}
		catch (...) {
			error = "Unhandled exception during headless runtime initialization";
			runtime.Shutdown();
			return false;
		}
	}

	~HeadlessRuntimeEnvironment() {
		Shutdown();
	}

	HeadlessMainThreadPump& MainThreadPump() {
		return main_thread_pump;
	}

private:
	void Shutdown() {
		runtime.Shutdown();
	}
};

template<typename Result, typename Start>
Result RunAsyncWithPump(HeadlessMainThreadPump& pump, Start&& start) {
	std::mutex mutex;
	std::optional<Result> result;

	start([&](Result finished) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			result.emplace(std::move(finished));
		}
		pump.Notify();
	});

	pump.RunUntil([&] {
		std::lock_guard<std::mutex> lock(mutex);
		return result.has_value();
	});

	std::lock_guard<std::mutex> lock(mutex);
	return std::move(*result);
}

int RunParsedHeadlessCli(HeadlessRuntimeEnvironment& runtime, headless_cli::ParseResult const& parsed) {
	if (!parsed.command)
		return 64;

	if (auto const* probe = std::get_if<headless_cli::ProbePlaybackCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_playback_probe::PlaybackProbeResult>(
			runtime.MainThreadPump(),
			[request = probe->request](auto&& on_done) mutable {
				aegisub::playback_probe_service::RunAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* session = std::get_if<headless_cli::SessionPlaybackCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_cli::PlaybackSessionResult>(
			runtime.MainThreadPump(),
			[request = session->request](auto&& on_done) mutable {
				headless_cli::RunSessionPlaybackAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* session = std::get_if<headless_cli::SessionProjectCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_cli::ProjectSessionResult>(
			runtime.MainThreadPump(),
			[request = session->request](auto&& on_done) mutable {
				headless_cli::RunSessionProjectAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* inspect = std::get_if<headless_cli::InspectTraceCommand>(&*parsed.command)) {
		auto result = headless_cli::RunInspectTrace(inspect->request);
		if (!result.output.empty())
			std::cout << result.output;
		if (!result.error.empty())
			std::cerr << result.error << std::endl;
		return result.exit_code;
	}

	if (auto const* inspect = std::get_if<headless_cli::InspectMediaCommand>(&*parsed.command)) {
		auto result = headless_cli::RunInspectMedia(inspect->request);
		std::cout << headless_cli::BuildMediaInspectJson(result);
		if (result.exit_code != 0 && !result.message.empty())
			std::cerr << result.message << std::endl;
		return result.exit_code;
	}

	if (auto const* inspect = std::get_if<headless_cli::InspectAssInfoCommand>(&*parsed.command)) {
		auto result = headless_cli::RunInspectAssInfo(inspect->request);
		std::cout << headless_cli::BuildAssInfoJson(result);
		if (!result.snapshot && !result.error.empty())
			std::cerr << result.error << std::endl;
		return result.snapshot ? 0 : 2;
	}

	if (auto const* batch = std::get_if<headless_cli::BatchPlaybackProbeCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_cli::BatchPlaybackProbeResult>(
			runtime.MainThreadPump(),
			[request = batch->request](auto&& on_done) mutable {
				headless_cli::RunBatchPlaybackProbeAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* batch = std::get_if<headless_cli::BatchTraceSummarizeCommand>(&*parsed.command)) {
		auto result = headless_cli::RunBatchTraceSummarize(batch->request);
		if (!result.message.empty())
			std::cout << result.message << std::endl;
		return result.exit_code;
	}

	if (auto const* batch = std::get_if<headless_cli::BatchAssInfoCommand>(&*parsed.command)) {
		auto result = headless_cli::RunBatchAssInfo(batch->request);
		if (!result.message.empty())
			std::cout << result.message << std::endl;
		return result.exit_code;
	}

	std::cerr << "unhandled CLI command" << std::endl;
	return 64;
}

int RunParsedLegacyProbe(HeadlessRuntimeEnvironment& runtime, headless_playback_probe::CommandLineParseResult const& parsed) {
	if (!parsed.request)
		return 64;

	auto result = RunAsyncWithPump<headless_playback_probe::PlaybackProbeResult>(
		runtime.MainThreadPump(),
		[request = *parsed.request](auto&& on_done) mutable {
			headless_playback_probe::RunAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
		});
	return result.exit_code;
}

}

bool IsHeadlessCommandLine(std::vector<std::string> const& args) {
	auto const cli_parse = headless_cli::ParseCommandLine(args);
	if (cli_parse.requested)
		return true;
	return headless_playback_probe::ParseCommandLine(args).requested;
}

int RunHeadlessCommandLine(std::vector<std::string> const& args) {
	auto const cli_parse = headless_cli::ParseCommandLine(args);
	if (cli_parse.requested) {
		if (!cli_parse.command) {
			std::cerr << cli_parse.error << std::endl;
			return 64;
		}

		HeadlessRuntimeEnvironment runtime;
		std::string error;
		if (!runtime.Initialize(error)) {
			ReportHeadlessError("headless-init", error);
			return 2;
		}
		return RunParsedHeadlessCli(runtime, cli_parse);
	}

	auto const probe_parse = headless_playback_probe::ParseCommandLine(args);
	if (probe_parse.requested) {
		if (!probe_parse.request) {
			std::cerr << probe_parse.error << std::endl;
			return 64;
		}

		HeadlessRuntimeEnvironment runtime;
		std::string error;
		if (!runtime.Initialize(error)) {
			ReportHeadlessError("headless-init", error);
			return 2;
		}
		return RunParsedLegacyProbe(runtime, probe_parse);
	}

	return 1;
}
