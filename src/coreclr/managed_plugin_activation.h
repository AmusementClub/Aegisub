#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agi::coreclr {

inline constexpr char kManagedPluginManifestFilename[] =
	"plugin.aegisub-plugin.json";

struct ManagedPluginActivationState {
	std::string plugin_id;
	bool enabled = true;
	std::string active_version;
	std::optional<std::string> previous_version;
	std::optional<std::string> last_failed_version;
	std::optional<std::string> pinned_version;
	uint64_t generation = 0;
};

struct ManagedPluginFallback {
	std::string version;
	agi::fs::path manifest_path;
};

struct ManagedPluginLoadCandidate {
	std::string plugin_id;
	std::string version;
	agi::fs::path manifest_path;
	std::optional<ManagedPluginFallback> fallback;
	uint64_t generation = 0;
	/// Set when discovery selected previous_version because active_version was
	/// already missing or invalid before the Automation engine attempted load.
	std::optional<std::string> rollback_from_version;
};

struct ManagedPluginActivationDiagnostic {
	std::string plugin_id;
	std::string message;
	bool error = true;
};

struct ManagedPluginDiscoveryResult {
	std::vector<ManagedPluginLoadCandidate> candidates;
	std::vector<ManagedPluginActivationDiagnostic> diagnostics;
};

/// Owns version selection state for locally installed managed plugins.
/// Package extraction is deliberately outside this type: callers first place
/// an immutable payload under VersionDirectory(), then atomically Activate it.
class ManagedPluginActivationStore final {
	agi::fs::path root;

public:
	explicit ManagedPluginActivationStore(agi::fs::path root);

	agi::fs::path const& Root() const noexcept { return root; }
	agi::fs::path PluginsDirectory() const;
	agi::fs::path StagingDirectory() const;
	agi::fs::path PluginDirectory(std::string const& plugin_id) const;
	agi::fs::path VersionDirectory(
		std::string const& plugin_id,
		std::string const& version) const;
	agi::fs::path ManifestPath(
		std::string const& plugin_id,
		std::string const& version) const;
	agi::fs::path ActivationPath(std::string const& plugin_id) const;

	/// Create a unique same-volume directory for package extraction.
	agi::fs::path CreateStagingDirectory(
		std::string const& plugin_id,
		std::string const& version) const;
	/// Validate and atomically publish a staged payload into the immutable
	/// version directory. Existing versions are never overwritten.
	agi::fs::path PublishStagedVersion(
		std::string const& plugin_id,
		std::string const& version,
		agi::fs::path const& staged_payload_directory) const;

	ManagedPluginActivationState ReadState(std::string const& plugin_id) const;
	std::optional<ManagedPluginActivationState> TryReadState(
		std::string const& plugin_id) const;

	/// Switch to an already materialized and validated local version. The old
	/// active version becomes previous_version for rollback.
	ManagedPluginActivationState Activate(
		std::string const& plugin_id,
		std::string const& version);
	ManagedPluginActivationState SetEnabled(
		std::string const& plugin_id,
		bool enabled);
	ManagedPluginActivationState SetPinnedVersion(
		std::string const& plugin_id,
		std::optional<std::string> version);
	/// Manual rollback swaps active_version and previous_version so it can be
	/// undone by another rollback.
	ManagedPluginActivationState Rollback(std::string const& plugin_id);

	ManagedPluginDiscoveryResult Discover() const;

	/// Persist an automatic fallback only if the state still has the generation
	/// and active version observed by discovery. Returns false for stale work.
	bool CommitAutomaticRollback(
		std::string const& plugin_id,
		uint64_t expected_generation,
		std::string const& failed_version,
		std::string const& fallback_version);
};

bool IsValidManagedPluginId(std::string const& value) noexcept;
bool IsValidManagedPluginVersion(std::string const& value) noexcept;

} // namespace agi::coreclr
