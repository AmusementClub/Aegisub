#include <gtest/gtest.h>

#include "../../src/automation_runtime_profile.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path UniqueTestPath(std::string const& label) {
	return std::filesystem::temp_directory_path()
		/ ("aegisub-profile-" + label + "-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(automation_runtime_profile, successful_owned_profile_is_removed) {
	std::string error;
	auto profile = AutomationRuntimeProfile::Create({}, error);
	ASSERT_TRUE(error.empty()) << error;
	auto root = profile.Root();
	ASSERT_TRUE(std::filesystem::exists(root));

	profile.Complete(true);
	EXPECT_FALSE(std::filesystem::exists(root));
}

TEST(automation_runtime_profile, failed_owned_profile_is_retained) {
	std::string error;
	auto profile = AutomationRuntimeProfile::Create({}, error);
	ASSERT_TRUE(error.empty()) << error;
	auto root = profile.Root();

	profile.Complete(false);
	EXPECT_TRUE(std::filesystem::exists(root));
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
}

TEST(automation_runtime_profile, explicit_profile_is_never_owned) {
	auto root = UniqueTestPath("explicit");
	std::string error;
	auto profile = AutomationRuntimeProfile::Create(
		AutomationRuntimeProfileOptions{root, std::nullopt, false}, error);
	ASSERT_TRUE(error.empty()) << error;
	ASSERT_TRUE(std::filesystem::exists(root / "user"));
	ASSERT_TRUE(std::filesystem::exists(root / "local"));

	profile.Complete(true);
	EXPECT_TRUE(std::filesystem::exists(root));
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
}

TEST(automation_runtime_profile, keep_profile_retains_successful_owned_profile) {
	std::string error;
	auto profile = AutomationRuntimeProfile::Create(
		AutomationRuntimeProfileOptions{std::nullopt, std::nullopt, true}, error);
	ASSERT_TRUE(error.empty()) << error;
	auto root = profile.Root();

	profile.Complete(true);
	EXPECT_TRUE(std::filesystem::exists(root));
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
}

}
