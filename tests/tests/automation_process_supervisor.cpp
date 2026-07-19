#include <gtest/gtest.h>

#include "../../src/automation_process_supervisor.h"
#include "../../src/automation_process_supervisor_test.h"
#include "../../src/automation_scenario.h"
#include "../../src/automation_scenario_runner.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace supervisor = aegisub::automation_process_supervisor;
namespace testing_hooks = aegisub::automation_process_supervisor::testing;

class automation_process_supervisor_test : public ::testing::Test {
protected:
	std::filesystem::path root;
	agi::fs::path control;

	void SetUp() override {
		root = std::filesystem::temp_directory_path()
			/ ("aegisub-supervisor-test-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root);
		control = root / "control";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}

	struct FakeChild {
		bool start_ok = true;
		std::string start_error = "start failed";
		bool exited = false;
		int exit_code = 0;
		int terminate_count = 0;
		bool started = false;
	};

	struct ManualClock {
		std::chrono::steady_clock::time_point now =
			std::chrono::steady_clock::time_point{1s};
	};

	testing_hooks::Hooks MakeHooks(FakeChild& child, ManualClock& clock) {
		testing_hooks::Hooks hooks;
		hooks.timeouts.startup = 1s;
		hooks.timeouts.teardown = 10s;
		hooks.timeouts.exit = 1s;
		hooks.start = [&](std::vector<std::string> const&, agi::fs::path const&, std::string& error) {
			if (!child.start_ok) {
				error = child.start_error;
				return false;
			}
			child.started = true;
			return true;
		};
		hooks.poll = [&](int& code) {
			if (!child.exited)
				return false;
			code = child.exit_code;
			return true;
		};
		hooks.terminate = [&] {
			++child.terminate_count;
			child.exited = true;
			child.exit_code = 1;
		};
		hooks.now = [&] {
			return clock.now;
		};
		// Advance time on sleep so the state machine progresses without real waits.
		hooks.sleep = [&](std::chrono::milliseconds duration) {
			clock.now += duration;
		};
		return hooks;
	}
};

TEST_F(automation_process_supervisor_test, start_failure_does_not_claim_started) {
	FakeChild child;
	child.start_ok = false;
	ManualClock clock;
	auto result = testing_hooks::Run(
		{"worker"}, control, 250, 1, MakeHooks(child, clock));
	EXPECT_FALSE(result.started);
	EXPECT_FALSE(result.timed_out);
	EXPECT_EQ(result.phase, "start");
	EXPECT_EQ(result.error, "start failed");
	EXPECT_EQ(child.terminate_count, 0);
}

TEST_F(automation_process_supervisor_test,
	scenario_complete_without_step_done_enters_teardown_not_step_timeout) {
	// Regression: schema/runtime failure never writes step-done. Short step
	// timeouts must not kill the worker during managed teardown.
	FakeChild child;
	ManualClock clock;
	auto hooks = MakeHooks(child, clock);
	hooks.timeouts.teardown = 10s;
	hooks.timeouts.exit = 1s;

	// Drive the monitor in a side thread-less cooperative way by writing markers
	// before each sleep advances time. Use a custom sleep that injects markers.
	bool marked_ready = false;
	bool marked_scenario = false;
	hooks.sleep = [&](std::chrono::milliseconds duration) {
		if (!marked_ready) {
			supervisor::MarkRuntimeReady(control);
			marked_ready = true;
		}
		else if (!marked_scenario) {
			// No step-done — only scenario-complete, as after ValidateScenario failure.
			supervisor::MarkWorkerScenarioComplete(control);
			marked_scenario = true;
		}
		clock.now += duration;
	};

	// Child never exits on its own → will hit teardown timeout after 10s budget.
	auto result = testing_hooks::Run(
		{"worker"}, control, /*step_timeout_ms=*/250, /*step_count=*/1, std::move(hooks));

	EXPECT_TRUE(result.started);
	EXPECT_TRUE(result.timed_out);
	EXPECT_EQ(result.phase, "teardown");
	EXPECT_EQ(result.error, "automation worker timed out during runtime teardown");
	EXPECT_EQ(child.terminate_count, 1);
	// Elapsed must exceed the short step timeout by a wide margin.
	// startup/step polls + 10s teardown (in 20ms sleeps) >> 250ms.
	EXPECT_GE(clock.now.time_since_epoch(), 10s);
}

TEST_F(automation_process_supervisor_test,
	delayed_shutdown_after_scenario_complete_is_allowed) {
	FakeChild child;
	ManualClock clock;
	auto hooks = MakeHooks(child, clock);
	hooks.timeouts.teardown = 10s;
	hooks.timeouts.exit = 1s;

	int sleep_calls = 0;
	hooks.sleep = [&](std::chrono::milliseconds duration) {
		++sleep_calls;
		if (sleep_calls == 1) {
			supervisor::MarkRuntimeReady(control);
			supervisor::MarkStep(control, 0, true);
		}
		else if (sleep_calls == 2) {
			supervisor::MarkStep(control, 0, false);
			supervisor::MarkWorkerScenarioComplete(control);
		}
		else if (sleep_calls == 3) {
			// Simulate ~8s of CoreCLR unload under a 10s teardown budget.
			clock.now += 8s;
			supervisor::MarkWorkerShutdownComplete(control);
		}
		else if (sleep_calls == 4) {
			child.exited = true;
			child.exit_code = 0;
		}
		clock.now += duration;
	};

	auto result = testing_hooks::Run(
		{"worker"}, control, 250, 1, std::move(hooks));

	EXPECT_TRUE(result.started);
	EXPECT_FALSE(result.timed_out);
	EXPECT_EQ(result.exit_code, 0);
	EXPECT_EQ(child.terminate_count, 0);
}

TEST_F(automation_process_supervisor_test,
	hang_after_shutdown_complete_uses_exit_budget) {
	FakeChild child;
	ManualClock clock;
	auto hooks = MakeHooks(child, clock);
	hooks.timeouts.teardown = 10s;
	hooks.timeouts.exit = 1s;

	int sleep_calls = 0;
	hooks.sleep = [&](std::chrono::milliseconds duration) {
		++sleep_calls;
		if (sleep_calls == 1) {
			supervisor::MarkRuntimeReady(control);
			supervisor::MarkWorkerScenarioComplete(control);
			supervisor::MarkWorkerShutdownComplete(control);
		}
		// Never exit — exit budget should fire.
		clock.now += duration;
	};

	auto result = testing_hooks::Run(
		{"worker"}, control, 5'000, 1, std::move(hooks));

	EXPECT_TRUE(result.timed_out);
	EXPECT_EQ(result.phase, "exit");
	EXPECT_EQ(result.error, "automation worker timed out after shutdown complete");
	EXPECT_EQ(child.terminate_count, 1);
}

TEST_F(automation_process_supervisor_test, active_step_timeout_keeps_step_phase) {
	FakeChild child;
	ManualClock clock;
	auto hooks = MakeHooks(child, clock);
	hooks.timeouts.teardown = 30s;

	int sleep_calls = 0;
	hooks.sleep = [&](std::chrono::milliseconds duration) {
		++sleep_calls;
		if (sleep_calls == 1) {
			supervisor::MarkRuntimeReady(control);
			supervisor::MarkStep(control, 0, true);
		}
		// Stay on step-running without scenario-complete.
		clock.now += duration;
	};

	auto result = testing_hooks::Run(
		{"worker"}, control, /*step_timeout_ms=*/100, 1, std::move(hooks));

	EXPECT_TRUE(result.timed_out);
	EXPECT_EQ(result.phase, "step");
	ASSERT_TRUE(result.step_index);
	EXPECT_EQ(*result.step_index, 0u);
	EXPECT_EQ(result.error, "automation scenario step 0 timed out");
}

TEST(automation_scenario_runner_phase, schema_error_includes_phase) {
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "schema-phase";
	scenario.hosts = {"headless"};
	json::Object step;
	step["action"] = "command";
	step["id"] = "edit/undo";
	scenario.steps.emplace_back(std::move(step));

	auto result = aegisub::automation_scenario_runner::Run(
		scenario,
		"headless",
		{},
		{}, // no automation executor
		{}); // no command executor → schema error for headless command

	EXPECT_EQ(result.exit_code, 64);
	EXPECT_FALSE(result.passed);
	ASSERT_FALSE(result.steps.empty());
	auto const& error = static_cast<json::Object const&>(result.steps.front());
	EXPECT_EQ(static_cast<json::String const&>(error.at("error_kind")), "schema");
	EXPECT_EQ(static_cast<json::String const&>(error.at("phase")), "scenario");
}

TEST(automation_scenario_runner_phase, runtime_error_includes_phase) {
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "runtime-phase";
	scenario.hosts = {"headless"};
	scenario.resources["script"] = agi::fs::path("dummy.lua");
	json::Object step;
	step["action"] = "run_automation";
	step["script"] = "script";
	step["macro"] = "DoesNotMatter";
	scenario.steps.emplace_back(std::move(step));

	auto result = aegisub::automation_scenario_runner::Run(
		scenario,
		"headless",
		{},
		[](auto) -> aegisub::automation_session_service::AutomationSessionResult {
			throw std::runtime_error("executor boom");
		},
		{});

	EXPECT_EQ(result.exit_code, 2);
	EXPECT_FALSE(result.passed);
	ASSERT_FALSE(result.steps.empty());
	auto const& error = static_cast<json::Object const&>(result.steps.front());
	EXPECT_EQ(static_cast<json::String const&>(error.at("error_kind")), "runtime");
	EXPECT_EQ(static_cast<json::String const&>(error.at("phase")), "scenario");
	EXPECT_EQ(static_cast<json::String const&>(error.at("error")), "executor boom");
}

}
