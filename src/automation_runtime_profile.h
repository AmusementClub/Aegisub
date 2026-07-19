#pragma once

#include "app_runtime.h"

#include <libaegisub/fs_fwd.h>

#include <optional>

struct AutomationRuntimeProfileOptions {
	std::optional<agi::fs::path> profile_directory;
	std::optional<agi::fs::path> artifacts_directory;
	bool keep_profile = false;
};

class AutomationRuntimeProfile final {
	agi::fs::path root;
	agi::fs::path user_directory;
	agi::fs::path local_directory;
	agi::fs::path artifacts_directory;
	bool owns_root = false;
	bool keep_profile = false;

	AutomationRuntimeProfile() = default;

public:
	static AutomationRuntimeProfile Create(
		AutomationRuntimeProfileOptions const& options,
		std::string& error);

	AutomationRuntimeProfile(AutomationRuntimeProfile const&) = delete;
	AutomationRuntimeProfile& operator=(AutomationRuntimeProfile const&) = delete;
	AutomationRuntimeProfile(AutomationRuntimeProfile&& other) noexcept;
	AutomationRuntimeProfile& operator=(AutomationRuntimeProfile&& other) noexcept;
	~AutomationRuntimeProfile();

	RuntimePathOverrides PathOverrides() const;
	agi::fs::path const& Root() const { return root; }
	agi::fs::path const& ArtifactsDirectory() const { return artifacts_directory; }
	void Complete(bool succeeded);
};
