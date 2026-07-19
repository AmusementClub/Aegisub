#include "automation_runtime_profile.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <filesystem>
#include <utility>

namespace {

agi::fs::path MakeUniqueDirectory(agi::fs::path const& parent, std::string const& prefix) {
	auto result = agi::fs::UniquePath(parent / agi::fs::PathFromString(prefix + "-%%%%%%%%"));
	agi::fs::CreateDirectory(result);
	return result;
}

void RemoveOwnedDirectory(agi::fs::path const& root) {
	if (root.empty())
		return;
	std::error_code error;
	std::filesystem::remove_all(root, error);
}

}

AutomationRuntimeProfile AutomationRuntimeProfile::Create(
	AutomationRuntimeProfileOptions const& options,
	std::string& error) {
	AutomationRuntimeProfile result;
	try {
		if (options.profile_directory) {
			result.root = std::filesystem::absolute(*options.profile_directory).lexically_normal();
			agi::fs::CreateDirectory(result.root);
		}
		else {
			result.root = MakeUniqueDirectory(
				std::filesystem::temp_directory_path(), "aegisub-automation");
			result.owns_root = true;
		}

		result.user_directory = result.root / agi::fs::PathFromString("user");
		result.local_directory = result.root / agi::fs::PathFromString("local");
		agi::fs::CreateDirectory(result.user_directory);
		agi::fs::CreateDirectory(result.local_directory);

		if (options.artifacts_directory)
			result.artifacts_directory = std::filesystem::absolute(*options.artifacts_directory).lexically_normal();
		else
			result.artifacts_directory = result.root / agi::fs::PathFromString("artifacts");
		agi::fs::CreateDirectory(result.artifacts_directory);
		result.keep_profile = options.keep_profile;
		return result;
	}
	catch (std::exception const& e) {
		error = e.what();
	}
	catch (...) {
		error = "could not create automation runtime profile";
	}

	if (result.owns_root)
		RemoveOwnedDirectory(result.root);
	return {};
}

AutomationRuntimeProfile::AutomationRuntimeProfile(AutomationRuntimeProfile&& other) noexcept
: root(std::move(other.root))
, user_directory(std::move(other.user_directory))
, local_directory(std::move(other.local_directory))
, artifacts_directory(std::move(other.artifacts_directory))
, owns_root(std::exchange(other.owns_root, false))
, keep_profile(other.keep_profile) {
}

AutomationRuntimeProfile& AutomationRuntimeProfile::operator=(AutomationRuntimeProfile&& other) noexcept {
	if (this == &other)
		return *this;
	if (owns_root && !keep_profile)
		RemoveOwnedDirectory(root);
	root = std::move(other.root);
	user_directory = std::move(other.user_directory);
	local_directory = std::move(other.local_directory);
	artifacts_directory = std::move(other.artifacts_directory);
	owns_root = std::exchange(other.owns_root, false);
	keep_profile = other.keep_profile;
	return *this;
}

AutomationRuntimeProfile::~AutomationRuntimeProfile() {
	Complete(false);
}

RuntimePathOverrides AutomationRuntimeProfile::PathOverrides() const {
	return {
		std::make_shared<agi::fs::path>(user_directory),
		std::make_shared<agi::fs::path>(local_directory),
		false};
}

void AutomationRuntimeProfile::Complete(bool succeeded) {
	if (!owns_root || root.empty())
		return;
	if (!succeeded && !keep_profile) {
		// Keep the failed profile for diagnostics, but detach ownership so the
		// destructor does not erase the evidence.
		owns_root = false;
		return;
	}
	if (!keep_profile)
		RemoveOwnedDirectory(root);
	root.clear();
	owns_root = false;
}
