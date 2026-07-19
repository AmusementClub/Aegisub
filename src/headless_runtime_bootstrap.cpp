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

#include "app_launch_plan.h"
#include "app_runtime.h"
#include "automation_process_supervisor.h"
#include "automation_runtime_profile.h"
#include "automation_scenario.h"
#include "automation_scenario_runner.h"
#include "headless_automation_cli.h"
#include "headless_cli_execute.h"
#include "headless_cli_parse.h"
#include "headless_playback_probe.h"
#include "automation_session_service.h"
#include "options.h"
#include "threaded_ui_timer.h"
#include "ui_services.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/path.h>

#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
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

class HeadlessNotificationSink final : public agi::NotificationSink {
public:
	void ShowInfo(std::string const&, std::string const&) override { }

	void ShowError(std::string const& title, std::string const& message) override {
		ReportHeadlessError(title, message);
	}

	void ShowWarning(std::string const& title, std::string const& message) override {
		ReportHeadlessError(title, message);
	}
};

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
	HeadlessNotificationSink notification_sink;

public:
	bool Initialize(
		std::string& error,
		RuntimePathOverrides path_overrides = {},
		bool initialize_commands = false) {
		try {
			AppRuntimeInitOptions options;
			options.shell_mode = RuntimeShellMode::Headless;
			options.locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
			options.path_overrides = std::move(path_overrides);
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
			options.ui_timer_host = CreateThreadedUiTimerHost();
			options.load_global_scripts = false;
			options.initialize_commands = initialize_commands;
			options.initialize_ui_locale = false;
			options.register_automation_script_factory = true;
			options.warm_subtitles_provider_font_cache = false;
			options.register_export_filters = false;
			options.install_png_handler = false;
			options.bootstrap_ui_host.notification_sink = &notification_sink;
			if (!runtime.Initialize(std::move(options), error))
				return false;

			// Headless contexts do not need wx autosave timers. Disabling them
			// avoids constructing timer owners on CLI-only runs.
			OPT_SET("App/Auto/Save")->SetBool(false);
			OPT_SET("App/Auto/Save Every Seconds")->SetInt(0);
			return true;
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
Result RunAsyncWithPump(HeadlessMainThreadPump& pump, Start&& start);

aegisub::automation_scenario_runner::Result RunHeadlessScenario(
	HeadlessRuntimeEnvironment& runtime,
	aegisub::automation_scenario::Scenario const& scenario,
	agi::fs::path const& artifacts,
	aegisub::automation_scenario_runner::StepObserver observe_step = {}) {
	return aegisub::automation_scenario_runner::Run(
		scenario,
		"headless",
		artifacts,
		[&](auto request) {
			return RunAsyncWithPump<aegisub::automation_session_service::AutomationSessionResult>(
				runtime.MainThreadPump(),
				[request = std::move(request)](auto&& on_done) mutable {
					aegisub::automation_session_service::RunAsync(
						std::move(request), std::forward<decltype(on_done)>(on_done));
				});
		},
		{},
		std::move(observe_step));
}

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

int WriteHeadlessScenarioResult(
	json::Object const& output,
	agi::fs::path const& artifacts) {
	std::ostringstream json_output;
	agi::JsonWriter::Write(output, json_output);
	std::cout << json_output.str() << std::endl;

	try {
		auto result_path = artifacts / agi::fs::PathFromString("result.json");
		auto result_stream = agi::io::Save(result_path);
		agi::JsonWriter::Write(output, result_stream.Get());
		return 0;
	}
	catch (std::exception const& e) {
		ReportHeadlessError("scenario-result", e.what());
		return 2;
	}
}

json::Object BuildHeadlessFailureOutput(
	std::string const& scenario_name,
	agi::fs::path const& profile,
	agi::fs::path const& artifacts,
	int exit_code,
	std::string const& error,
	std::string const& error_kind,
	std::string const& phase,
	bool timed_out = false,
	std::optional<std::size_t> step_index = std::nullopt) {
	aegisub::automation_scenario_runner::Result execution;
	execution.exit_code = exit_code;
	execution.passed = false;
	json::Object step;
	step["error"] = error;
	step["error_kind"] = error_kind;
	step["phase"] = phase;
	if (timed_out)
		step["timed_out"] = true;
	if (step_index)
		step["step_index"] = static_cast<int64_t>(*step_index);
	execution.steps.emplace_back(std::move(step));
	return aegisub::automation_scenario_runner::SerializeResult(
		scenario_name,
		"headless",
		std::move(execution),
		profile,
		artifacts);
}

bool PrintHeadlessScenarioResult(agi::fs::path const& artifacts) {
	try {
		auto stream = agi::io::Open(
			artifacts / agi::fs::PathFromString("result.json"));
		std::cout << stream->rdbuf() << std::endl;
		return true;
	}
	catch (...) {
		return false;
	}
}

int FinishHeadlessWorker(
	AutomationRuntimeProfile& profile,
	json::Object output,
	bool passed,
	int exit_code,
	agi::fs::path const& control_directory) {
	if (auto const write_result = WriteHeadlessScenarioResult(
		output, profile.ArtifactsDirectory())) {
		// Still emit the shutdown marker so the parent leaves the teardown budget.
		try {
			aegisub::automation_process_supervisor::MarkWorkerShutdownComplete(
				control_directory);
		}
		catch (...) {
		}
		profile.Complete(false);
		return write_result;
	}

	try {
		aegisub::automation_process_supervisor::MarkWorkerShutdownComplete(
			control_directory);
	}
	catch (std::exception const& e) {
		ReportHeadlessError("automation-worker-marker", e.what());
	}

	profile.Complete(passed);
	return passed ? 0 : exit_code == 0 ? 1 : exit_code;
}

int RunNewHeadlessAutomationWorker(
	aegisub::headless_automation_cli::RunRequest const& request,
	aegisub::automation_scenario::Scenario const& scenario,
	AutomationRuntimeProfile& profile) {
	auto const control_directory = profile.ArtifactsDirectory()
		/ agi::fs::PathFromString(".automation-control");
	aegisub::automation_scenario_runner::Result execution;
	std::string runtime_error;
	std::string worker_error;
	std::string failure_phase;
	bool runtime_ready_marked = false;

	{
		HeadlessRuntimeEnvironment runtime;
		if (!runtime.Initialize(runtime_error, profile.PathOverrides(), false)) {
			ReportHeadlessError("headless-init", runtime_error);
			failure_phase = "init";
		}
		else {
			try {
				aegisub::automation_process_supervisor::MarkRuntimeReady(control_directory);
				runtime_ready_marked = true;
				execution = RunHeadlessScenario(
					runtime,
					scenario,
					profile.ArtifactsDirectory(),
					[control_directory](std::size_t index, bool started) {
						aegisub::automation_process_supervisor::MarkStep(
							control_directory, index, started);
					});
			}
			catch (std::exception const& e) {
				ReportHeadlessError("automation-worker", e.what());
				worker_error = e.what();
				failure_phase = "scenario";
			}
			catch (...) {
				ReportHeadlessError("automation-worker", "unknown automation worker failure");
				worker_error = "unknown automation worker failure";
				failure_phase = "scenario";
			}
		}

		// Scenario work is finished (success, schema failure, or caught exception).
		// Switch the supervisor onto the long teardown budget *before* destroying
		// the runtime — including paths that never wrote step-N-done.
		if (runtime_ready_marked) {
			try {
				aegisub::automation_process_supervisor::MarkWorkerScenarioComplete(
					control_directory);
			}
			catch (std::exception const& e) {
				ReportHeadlessError("automation-worker-marker", e.what());
			}
		}
		// Leaving this scope tears down HeadlessRuntimeEnvironment (managed
		// plugins / CoreCLR / font cache / dispatch). Supervisor uses the
		// teardown budget until worker-shutdown-complete below.
	}

	if (!failure_phase.empty()) {
		auto const exit_code = 2;
		auto output = BuildHeadlessFailureOutput(
			scenario.name,
			profile.Root(),
			profile.ArtifactsDirectory(),
			exit_code,
			failure_phase == "init" ? runtime_error : worker_error,
			failure_phase == "init" ? "init" : "runtime",
			failure_phase);
		return FinishHeadlessWorker(
			profile, std::move(output), false, exit_code, control_directory);
	}

	auto const passed = execution.passed;
	auto const exit_code = execution.exit_code;
	auto output = aegisub::automation_scenario_runner::SerializeResult(
		scenario.name,
		"headless",
		std::move(execution),
		profile.Root(),
		profile.ArtifactsDirectory());
	return FinishHeadlessWorker(
		profile, std::move(output), passed, exit_code, control_directory);
}

int RunNewHeadlessAutomation(
	aegisub::headless_automation_cli::RunRequest const& request,
	std::vector<std::string> const& original_args) {
	auto scenario_result = aegisub::automation_scenario::Load(request.scenario_path, request.inputs);
	if (!scenario_result.scenario) {
		ReportHeadlessError("scenario-parse", scenario_result.error);
		return 64;
	}

	std::string profile_error;
	auto profile = AutomationRuntimeProfile::Create(
		AutomationRuntimeProfileOptions{
			request.profile_directory,
			request.artifacts_directory,
			request.keep_profile},
		profile_error);
	if (profile.Root().empty()) {
		ReportHeadlessError("automation-profile", profile_error);
		return 2;
	}

	if (request.internal_worker)
		return RunNewHeadlessAutomationWorker(
			request, *scenario_result.scenario, profile);

	auto worker_args = original_args;
	worker_args.emplace_back("--automation-worker");
	worker_args.emplace_back("--profile-dir");
	worker_args.emplace_back(agi::fs::PathToString(profile.Root()));
	worker_args.emplace_back("--artifacts");
	worker_args.emplace_back(agi::fs::PathToString(profile.ArtifactsDirectory()));

	auto const control_directory = profile.ArtifactsDirectory()
		/ agi::fs::PathFromString(".automation-control");
	auto supervised = aegisub::automation_process_supervisor::Run(
		worker_args,
		control_directory,
		scenario_result.scenario->default_timeout_ms,
		scenario_result.scenario->steps.size());
	if (supervised.timed_out || !supervised.started) {
		auto output = BuildHeadlessFailureOutput(
			scenario_result.scenario->name,
			profile.Root(),
			profile.ArtifactsDirectory(),
			supervised.timed_out ? 1 : 2,
			supervised.error.empty()
				? (supervised.timed_out
					? "automation worker timed out"
					: "automation worker failed to start")
				: supervised.error,
			supervised.timed_out ? "timeout" : "start",
			supervised.phase.empty()
				? (supervised.timed_out ? "unknown" : "start")
				: supervised.phase,
			supervised.timed_out,
			supervised.step_index);
		if (WriteHeadlessScenarioResult(output, profile.ArtifactsDirectory()) == 2)
			supervised.exit_code = 2;
		profile.Complete(false);
		return supervised.exit_code;
	}

	if (!PrintHeadlessScenarioResult(profile.ArtifactsDirectory())) {
		auto output = BuildHeadlessFailureOutput(
			scenario_result.scenario->name,
			profile.Root(),
			profile.ArtifactsDirectory(),
			2,
			"automation worker exited without writing result.json",
			"missing_result",
			supervised.phase.empty() ? "exit" : supervised.phase);
		WriteHeadlessScenarioResult(output, profile.ArtifactsDirectory());
		profile.Complete(false);
		return 2;
	}

	profile.Complete(supervised.exit_code == 0);
	return supervised.exit_code;
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

	if (auto const* session = std::get_if<headless_cli::SessionAutomationCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_cli::AutomationSessionResult>(
			runtime.MainThreadPump(),
			[request = session->request](auto&& on_done) mutable {
				headless_cli::RunSessionAutomationAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
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

int RunHeadlessLaunchPlan(AppLaunchPlan const& plan) {
	if (!plan.RequestedHeadless())
		return 1;

	auto const& args = plan.legacy_headless_args.empty()
		? plan.original_args
		: plan.legacy_headless_args;
	if (plan.legacy_headless_args.empty()
		&& args.size() > 1 && args[1] == "--headless") {
		if (!plan.headless_run) {
			std::cerr << plan.error << std::endl;
			return 64;
		}
		return RunNewHeadlessAutomation(*plan.headless_run, args);
	}

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
