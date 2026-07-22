#include <gtest/gtest.h>

#include "../../src/automation_scenario.h"
#include "../../src/automation_scenario_runner.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

class automation_scenario_contract_test : public ::testing::Test {
protected:
	std::filesystem::path root;
	agi::fs::path scenario_path;

	void SetUp() override {
		root = std::filesystem::temp_directory_path()
			/ ("aegisub-scenario-test-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root);
		scenario_path = root / "scenario.json";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}

	void Write(std::string const& json) {
		std::ofstream stream(scenario_path, std::ios::binary);
		stream << json;
	}
};

json::Object Step(std::string const& action) {
	json::Object step;
	step["action"] = action;
	return step;
}

std::string JsonString(json::Object const& object, std::string const& key) {
	return static_cast<json::String const&>(object.at(key));
}

bool JsonBool(json::Object const& object, std::string const& key) {
	return static_cast<json::Boolean const&>(object.at(key));
}

TEST_F(automation_scenario_contract_test, global_scripts_are_opt_in) {
	Write(R"({
		"version": 1,
		"name": "default-policy",
		"steps": [{"action": "wait", "milliseconds": 1}]
	})");
	auto defaults = aegisub::automation_scenario::Load(scenario_path, {});
	ASSERT_TRUE(defaults.scenario) << defaults.error;
	EXPECT_FALSE(defaults.scenario->load_global_scripts);

	Write(R"({
		"version": 1,
		"name": "explicit-policy",
		"load_global_scripts": true,
		"steps": [{"action": "wait", "milliseconds": 1}]
	})");
	auto explicit_policy = aegisub::automation_scenario::Load(scenario_path, {});
	ASSERT_TRUE(explicit_policy.scenario) << explicit_policy.error;
	EXPECT_TRUE(explicit_policy.scenario->load_global_scripts);
}

TEST_F(automation_scenario_contract_test, global_script_policy_must_be_boolean) {
	Write(R"({
		"version": 1,
		"name": "invalid-policy",
		"load_global_scripts": "yes",
		"steps": [{"action": "wait", "milliseconds": 1}]
	})");

	auto loaded = aegisub::automation_scenario::Load(scenario_path, {});
	EXPECT_FALSE(loaded.scenario);
	EXPECT_NE(loaded.error.find("must be a boolean"), std::string::npos);
}

TEST_F(automation_scenario_contract_test, resources_resolve_relative_to_scenario_and_accept_known_override) {
	Write(R"({
		"version": 1,
		"name": "resources",
		"resources": {"script": "scripts/test.lua"},
		"steps": [{"action": "run_automation", "script": "script", "macro": "Test"}]
	})");

	auto loaded = aegisub::automation_scenario::Load(scenario_path, {});
	ASSERT_TRUE(loaded.scenario) << loaded.error;
	EXPECT_EQ(
		loaded.scenario->resources.at("script"),
		(root / "scripts" / "test.lua").lexically_normal());

	auto override_path = root / "override.lua";
	auto overridden = aegisub::automation_scenario::Load(
		scenario_path,
		{{"script", agi::fs::PathToString(override_path)}});
	ASSERT_TRUE(overridden.scenario) << overridden.error;
	EXPECT_EQ(overridden.scenario->resources.at("script"), override_path.lexically_normal());
}

TEST_F(automation_scenario_contract_test, rejects_unknown_resource_override) {
	Write(R"({
		"version": 1,
		"name": "resources",
		"resources": {"script": "test.lua"},
		"steps": [{"action": "run_automation", "script": "script", "macro": "Test"}]
	})");

	auto loaded = aegisub::automation_scenario::Load(
		scenario_path, {{"subtitle", "test.ass"}});
	EXPECT_FALSE(loaded.scenario);
	EXPECT_NE(loaded.error.find("unknown automation scenario input"), std::string::npos);
}

TEST(automation_scenario_runner_contract, query_assert_wait_and_capture_execute_in_order) {
	using namespace std::chrono_literals;
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "gui-contract";
	scenario.hosts = {"gui-test"};

	auto query = Step("query");
	query["target"] = "command";
	query["id"] = "test/toggle";
	scenario.steps.emplace_back(std::move(query));
	auto assertion = Step("assert");
	assertion["target"] = "command";
	assertion["id"] = "test/toggle";
	assertion["field"] = "validated";
	assertion["equals"] = true;
	scenario.steps.emplace_back(std::move(assertion));
	auto wait = Step("wait");
	wait["milliseconds"] = static_cast<int64_t>(17);
	scenario.steps.emplace_back(std::move(wait));
	auto capture = Step("capture");
	capture["name"] = "main-window";
	scenario.steps.emplace_back(std::move(capture));

	int query_count = 0;
	std::chrono::milliseconds waited{};
	agi::fs::path captured_path;
	auto result = aegisub::automation_scenario_runner::Run(
		scenario,
		"gui-test",
		agi::fs::path("artifacts"),
		{},
		{},
		{},
		[&](std::string const& target, json::Object const& step) {
			++query_count;
			EXPECT_EQ(target, "command");
			EXPECT_EQ(JsonString(step, "id"), "test/toggle");
			json::Object state;
			state["found"] = true;
			state["validated"] = true;
			return state;
		},
		[&](std::chrono::milliseconds duration) { waited = duration; },
		[&](std::string const& name, agi::fs::path const& output) {
			EXPECT_EQ(name, "main-window");
			captured_path = output;
			json::Object capture_result;
			capture_result["captured"] = true;
			return capture_result;
		});

	EXPECT_TRUE(result.passed);
	EXPECT_EQ(result.exit_code, 0);
	EXPECT_EQ(result.steps.size(), 4u);
	EXPECT_EQ(query_count, 2);
	EXPECT_EQ(waited, 17ms);
	EXPECT_EQ(captured_path, agi::fs::path("artifacts") / "main-window.png");
	auto const& assertion_result = static_cast<json::Object const&>(result.steps[1]);
	EXPECT_TRUE(JsonBool(assertion_result, "matched"));
}

TEST(automation_scenario_runner_contract, failed_assertion_stops_the_scenario) {
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "failed-assert";
	scenario.hosts = {"gui-test"};
	auto assertion = Step("assert");
	assertion["target"] = "command";
	assertion["id"] = "test/toggle";
	assertion["field"] = "active";
	assertion["equals"] = true;
	scenario.steps.emplace_back(std::move(assertion));

	auto result = aegisub::automation_scenario_runner::Run(
		scenario, "gui-test", {}, {}, {}, {},
		[](std::string const&, json::Object const&) {
			json::Object state;
			state["active"] = false;
			return state;
		});

	EXPECT_FALSE(result.passed);
	EXPECT_EQ(result.exit_code, 1);
	ASSERT_EQ(result.steps.size(), 1u);
	auto const& assertion_result = static_cast<json::Object const&>(result.steps.front());
	EXPECT_FALSE(JsonBool(assertion_result, "matched"));
	EXPECT_EQ(JsonString(assertion_result, "status"), "failed");
}

TEST(automation_scenario_runner_contract, rejects_unknown_query_target_before_execution) {
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "bad-target";
	scenario.hosts = {"gui-test"};
	auto query = Step("query");
	query["target"] = "window";
	query["id"] = "main";
	scenario.steps.emplace_back(std::move(query));
	bool called = false;

	auto result = aegisub::automation_scenario_runner::Run(
		scenario, "gui-test", {}, {}, {}, {},
		[&](std::string const&, json::Object const&) {
			called = true;
			return json::Object{};
		});

	EXPECT_FALSE(called);
	EXPECT_EQ(result.exit_code, 64);
	EXPECT_FALSE(result.passed);
}

TEST(automation_scenario_runner_contract, rejects_capture_path_traversal) {
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "bad-capture";
	scenario.hosts = {"gui-test"};
	auto capture = Step("capture");
	capture["name"] = "../outside";
	scenario.steps.emplace_back(std::move(capture));

	auto result = aegisub::automation_scenario_runner::Run(
		scenario, "gui-test", {}, {}, {}, {}, {}, {},
		[](std::string const&, agi::fs::path const&) { return json::Object{}; });

	EXPECT_EQ(result.exit_code, 64);
	EXPECT_FALSE(result.passed);
}

TEST(automation_scenario_runner_contract, capture_must_report_explicit_success) {
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "capture-status";
	scenario.hosts = {"gui-test"};
	auto capture = Step("capture");
	capture["name"] = "main-window";
	scenario.steps.emplace_back(std::move(capture));

	auto result = aegisub::automation_scenario_runner::Run(
		scenario, "gui-test", {}, {}, {}, {}, {}, {},
		[](std::string const&, agi::fs::path const&) { return json::Object{}; });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_FALSE(result.passed);
}

TEST(automation_scenario_runner_contract, reports_step_timeout_with_a_finite_total_budget) {
	using namespace std::chrono_literals;
	aegisub::automation_scenario::Scenario scenario;
	scenario.version = 1;
	scenario.name = "timeout";
	scenario.hosts = {"gui-test"};
	scenario.default_timeout_ms = 1;
	auto wait = Step("wait");
	wait["milliseconds"] = static_cast<int64_t>(1);
	scenario.steps.emplace_back(std::move(wait));

	auto result = aegisub::automation_scenario_runner::Run(
		scenario, "gui-test", {}, {}, {}, {}, {},
		[](std::chrono::milliseconds) { std::this_thread::sleep_for(10ms); });

	EXPECT_FALSE(result.passed);
	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(result.timed_out);
	ASSERT_EQ(result.timed_out_step, 0u);
	ASSERT_EQ(result.steps.size(), 2u);
	auto const& timeout = static_cast<json::Object const&>(result.steps.back());
	EXPECT_EQ(JsonString(timeout, "error_kind"), "timeout");
	EXPECT_TRUE(JsonBool(timeout, "timed_out"));

	auto serialized = aegisub::automation_scenario_runner::SerializeResult(
		scenario.name, "gui-test", std::move(result), {}, {});
	EXPECT_TRUE(JsonBool(serialized, "timed_out"));
}

}
