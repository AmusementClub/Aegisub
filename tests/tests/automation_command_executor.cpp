#include <gtest/gtest.h>

#include "../../src/automation_command_executor.h"
#include "../../src/command/command.h"
#include "../../src/include/aegisub/context.h"

#include <memory>
#include <string>

namespace {
cmd::Command* registered_command = nullptr;
}

namespace cmd {
Command* get_if(std::string const& name) {
	return registered_command && name == registered_command->name()
		? registered_command
		: nullptr;
}
}

namespace agi {
std::shared_ptr<StatusSink> Context::GetStatusSink() const {
	return {};
}
}

namespace {

class TestCommand final : public cmd::Command {
public:
	bool valid = true;
	bool active = false;
	bool invoked = false;
	cmd::CommandExecutionScope scope = cmd::CommandExecutionScope::GuiOnly;

	const char* name() const override { return "test/toggle"; }
	wxString StrMenu(agi::Context const*) const override { return wxS("Test"); }
	wxString StrDisplay(agi::Context const*) const override { return wxS("Test"); }
	wxString StrHelp() const override { return wxS("Test command"); }
	int Type() const override { return cmd::COMMAND_TOGGLE | cmd::COMMAND_VALIDATE; }
	bool Validate(agi::Context const*) override { return valid; }
	bool IsActive(agi::Context const*) override { return active; }
	cmd::CommandExecutionScope AutomationScope() const noexcept override { return scope; }
	void operator()(agi::Context*) override {
		invoked = true;
		active = !active;
	}
};

bool JsonBool(json::Object const& object, std::string const& key) {
	return static_cast<json::Boolean const&>(object.at(key));
}

std::string JsonString(json::Object const& object, std::string const& key) {
	return static_cast<json::String const&>(object.at(key));
}

class automation_command_executor_test : public ::testing::Test {
protected:
	TestCommand command;

	void SetUp() override { registered_command = &command; }
	void TearDown() override { registered_command = nullptr; }
};

TEST_F(automation_command_executor_test, inspect_reports_missing_command) {
	auto result = aegisub::automation_command_executor::Inspect("missing", nullptr, true);

	EXPECT_FALSE(JsonBool(result, "found"));
	EXPECT_FALSE(JsonBool(result, "allowed"));
	EXPECT_FALSE(JsonBool(result, "validated"));
}

TEST_F(automation_command_executor_test, inspect_rejects_gui_only_command_for_headless) {
	auto result = aegisub::automation_command_executor::Inspect(command.name(), nullptr, false);

	EXPECT_TRUE(JsonBool(result, "found"));
	EXPECT_FALSE(JsonBool(result, "allowed"));
	EXPECT_FALSE(JsonBool(result, "validated"));
	EXPECT_EQ(JsonString(result, "scope"), "gui_only");
}

TEST_F(automation_command_executor_test, inspect_reports_validation_and_active_state) {
	command.scope = cmd::CommandExecutionScope::HeadlessSafe;
	command.active = true;
	auto result = aegisub::automation_command_executor::Inspect(command.name(), nullptr, false);

	EXPECT_TRUE(JsonBool(result, "found"));
	EXPECT_TRUE(JsonBool(result, "allowed"));
	EXPECT_TRUE(JsonBool(result, "validated"));
	EXPECT_TRUE(JsonBool(result, "active"));
	EXPECT_EQ(JsonString(result, "scope"), "headless_safe");
}

TEST_F(automation_command_executor_test, invoke_reuses_policy_and_reports_toggle_transition) {
	command.scope = cmd::CommandExecutionScope::HeadlessSafe;
	auto result = aegisub::automation_command_executor::Invoke(command.name(), nullptr, false);

	EXPECT_TRUE(command.invoked);
	EXPECT_TRUE(JsonBool(result, "invoked"));
	EXPECT_FALSE(JsonBool(result, "active_before"));
	EXPECT_TRUE(JsonBool(result, "active_after"));
}

TEST_F(automation_command_executor_test, invalid_command_is_not_invoked) {
	command.scope = cmd::CommandExecutionScope::HeadlessSafe;
	command.valid = false;
	auto result = aegisub::automation_command_executor::Invoke(command.name(), nullptr, false);

	EXPECT_FALSE(command.invoked);
	EXPECT_FALSE(JsonBool(result, "invoked"));
	EXPECT_FALSE(JsonBool(result, "validated"));
}

}
