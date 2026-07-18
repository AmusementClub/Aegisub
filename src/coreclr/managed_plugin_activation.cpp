#include "managed_plugin_activation.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef CreateDirectory
#endif

namespace agi::coreclr {
namespace {

constexpr int64_t kActivationSchemaVersion = 1;

bool IsAsciiLowerAlphaNumeric(char value) noexcept {
	return (value >= 'a' && value <= 'z') ||
		(value >= '0' && value <= '9');
}

bool IsAsciiAlphaNumeric(char value) noexcept {
	return IsAsciiLowerAlphaNumeric(value) ||
		(value >= 'A' && value <= 'Z');
}

json::Object const& AsObject(json::UnknownElement const& value, char const* source) {
	try { return static_cast<json::Object const&>(value); }
	catch (...) { throw std::runtime_error(std::string(source) + " must be an object"); }
}

json::Object ParseObjectFile(agi::fs::path const& path, char const* source) {
	auto stream = agi::io::Open(path);
	json::UnknownElement root;
	json::Reader::Read(root, *stream);
	try {
		auto& object = static_cast<json::Object&>(root);
		return std::move(object);
	}
	catch (...) {
		throw std::runtime_error(std::string(source) + " must contain a JSON object");
	}
}

std::string RequireString(
	json::Object const& object,
	std::string const& key,
	char const* source) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(std::string(source) + " requires '" + key + "'");
	try {
		auto result = static_cast<json::String const&>(it->second);
		if (result.empty())
			throw std::runtime_error(
				std::string(source) + " field '" + key + "' cannot be empty");
		return result;
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) {
		throw std::runtime_error(
			std::string(source) + " field '" + key + "' must be a string");
	}
}

bool RequireBool(
	json::Object const& object,
	std::string const& key,
	char const* source) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(std::string(source) + " requires '" + key + "'");
	try { return static_cast<json::Boolean const&>(it->second); }
	catch (...) {
		throw std::runtime_error(
			std::string(source) + " field '" + key + "' must be boolean");
	}
}

int64_t RequireInteger(
	json::Object const& object,
	std::string const& key,
	char const* source) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(std::string(source) + " requires '" + key + "'");
	try { return static_cast<json::Integer const&>(it->second); }
	catch (...) {
		throw std::runtime_error(
			std::string(source) + " field '" + key + "' must be an integer");
	}
}

std::optional<std::string> OptionalString(
	json::Object const& object,
	std::string const& key,
	char const* source) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	try {
		(void)static_cast<json::Null const&>(it->second);
		return std::nullopt;
	}
	catch (...) { }
	try {
		auto result = static_cast<json::String const&>(it->second);
		if (result.empty())
			throw std::runtime_error(
				std::string(source) + " field '" + key + "' cannot be empty");
		return result;
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) {
		throw std::runtime_error(
			std::string(source) + " field '" + key + "' must be a string or null");
	}
}

ManagedPluginActivationState ParseState(json::Object const& object) {
	if (RequireInteger(object, "schemaVersion", "Managed plugin activation") !=
		kActivationSchemaVersion)
		throw std::runtime_error("Unsupported managed plugin activation schemaVersion");

	ManagedPluginActivationState state;
	state.plugin_id = RequireString(object, "pluginId", "Managed plugin activation");
	state.enabled = RequireBool(object, "enabled", "Managed plugin activation");
	state.active_version = RequireString(
		object, "activeVersion", "Managed plugin activation");
	state.previous_version = OptionalString(
		object, "previousVersion", "Managed plugin activation");
	state.last_failed_version = OptionalString(
		object, "lastFailedVersion", "Managed plugin activation");
	state.pinned_version = OptionalString(
		object, "pinnedVersion", "Managed plugin activation");
	auto generation = RequireInteger(object, "generation", "Managed plugin activation");
	if (generation <= 0)
		throw std::runtime_error("Managed plugin activation generation must be positive");
	state.generation = static_cast<uint64_t>(generation);

	if (!IsValidManagedPluginId(state.plugin_id))
		throw std::runtime_error("Managed plugin activation contains an invalid plugin ID");
	auto validate_version = [](std::optional<std::string> const& value) {
		return !value || IsValidManagedPluginVersion(*value);
	};
	if (!IsValidManagedPluginVersion(state.active_version) ||
		!validate_version(state.previous_version) ||
		!validate_version(state.last_failed_version) ||
		!validate_version(state.pinned_version))
		throw std::runtime_error("Managed plugin activation contains an invalid version");
	if (state.previous_version && *state.previous_version == state.active_version)
		throw std::runtime_error(
			"Managed plugin activation previousVersion must differ from activeVersion");
	return state;
}

json::UnknownElement OptionalJsonString(std::optional<std::string> const& value) {
	if (value) return json::UnknownElement(*value);
	return json::UnknownElement(json::Null{});
}

void WriteState(
	agi::fs::path const& path,
	ManagedPluginActivationState const& state) {
	json::Object root;
	root["schemaVersion"] = kActivationSchemaVersion;
	root["pluginId"] = state.plugin_id;
	root["enabled"] = state.enabled;
	root["activeVersion"] = state.active_version;
	root["previousVersion"] = OptionalJsonString(state.previous_version);
	root["lastFailedVersion"] = OptionalJsonString(state.last_failed_version);
	root["pinnedVersion"] = OptionalJsonString(state.pinned_version);
	if (state.generation > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		throw std::overflow_error("Managed plugin activation generation is exhausted");
	root["generation"] = static_cast<int64_t>(state.generation);

	agi::fs::CreateDirectory(path.parent_path());
	agi::io::Save output(path);
	agi::JsonWriter::Write(root, output.Get());
	output.Close();
}

void ValidateManifest(
	agi::fs::path const& path,
	std::string const& plugin_id,
	std::string const& version) {
	if (!agi::fs::FileExists(path))
		throw std::runtime_error(
			"Managed plugin version does not contain " +
			std::string(kManagedPluginManifestFilename));
	auto object = ParseObjectFile(path, "Managed plugin manifest");
	if (RequireInteger(object, "manifestVersion", "Managed plugin manifest") != 2)
		throw std::runtime_error("Managed plugin activation requires manifestVersion 2");
	if (RequireString(object, "id", "Managed plugin manifest") != plugin_id)
		throw std::runtime_error("Managed plugin manifest ID does not match its activation");
	if (RequireString(object, "version", "Managed plugin manifest") != version)
		throw std::runtime_error("Managed plugin manifest version does not match its directory");
	auto runtime_it = object.find("runtime");
	if (runtime_it == object.end())
		throw std::runtime_error("Managed plugin manifest requires a runtime object");
	auto const& runtime = AsObject(runtime_it->second, "Managed plugin runtime");
	auto runtime_kind = RequireString(runtime, "kind", "Managed plugin runtime");
	if (runtime_kind != "coreclr" && runtime_kind != "native")
		throw std::runtime_error(
			"Unsupported managed plugin runtime kind '" + runtime_kind + "'");
}

std::string DescribeException() {
	try { throw; }
	catch (std::exception const& error) { return error.what(); }
	catch (...) { return "Unknown managed plugin activation error"; }
}

uint64_t NextGeneration(uint64_t generation) {
	if (generation >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		throw std::overflow_error("Managed plugin activation generation is exhausted");
	return generation + 1;
}

bool IsWithinDirectory(
	agi::fs::path const& candidate,
	agi::fs::path const& directory) {
	auto candidate_it = candidate.begin();
	for (auto directory_it = directory.begin(); directory_it != directory.end();
		++directory_it, ++candidate_it) {
		if (candidate_it == candidate.end() || *candidate_it != *directory_it)
			return false;
	}
	return candidate_it != candidate.end();
}

bool IsLinkLikeEntry(
	agi::fs::path const& path,
	std::filesystem::file_status const& status) {
	if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
	auto const attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
		throw std::runtime_error(
			"Could not inspect a managed plugin filesystem entry");
	return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	(void)path;
	return false;
#endif
}

void RejectLinkLikeEntry(agi::fs::path const& path, char const* description) {
	std::error_code error;
	auto const status = std::filesystem::symlink_status(path, error);
	if (error)
		throw std::runtime_error(
			std::string("Could not inspect ") + description);
	if (IsLinkLikeEntry(path, status))
		throw std::runtime_error(
			std::string(description) +
			" cannot be a symbolic link or reparse point");
}

void EnsurePlainDirectory(agi::fs::path const& path, char const* description) {
	if (!agi::fs::Exists(path)) agi::fs::CreateDirectory(path);
	if (!agi::fs::DirectoryExists(path))
		throw std::runtime_error(std::string(description) + " must be a directory");
	RejectLinkLikeEntry(path, description);
}

void ValidateInstalledManifest(
	agi::fs::path const& path,
	agi::fs::path const& plugins_directory,
	std::string const& plugin_id,
	std::string const& version) {
	auto const version_directory = path.parent_path();
	RejectLinkLikeEntry(version_directory, "Managed plugin version directory");
	auto const canonical_version_directory =
		agi::fs::Canonicalize(version_directory);
	auto const canonical_plugins_directory =
		agi::fs::Canonicalize(plugins_directory);
	if (!IsWithinDirectory(
			canonical_version_directory, canonical_plugins_directory))
		throw std::runtime_error(
			"Managed plugin version directory escapes its store");
	ValidateManifest(path, plugin_id, version);
}

void RejectLinkedStagedEntries(agi::fs::path const& directory) {
	std::error_code error;
	for (std::filesystem::recursive_directory_iterator iterator(
			directory,
			std::filesystem::directory_options::skip_permission_denied,
			error), end;
		iterator != end;
		iterator.increment(error)) {
		if (error)
			throw std::runtime_error(
				"Could not inspect all files in the managed plugin staging directory");
		auto status = iterator->symlink_status(error);
		if (error)
			throw std::runtime_error(
				"Could not inspect a managed plugin staging entry");
		if (IsLinkLikeEntry(iterator->path(), status))
			throw std::runtime_error(
				"Managed plugin staging directories cannot contain symbolic links or reparse points");
	}
	if (error)
		throw std::runtime_error(
			"Could not enumerate the managed plugin staging directory");
}

} // namespace

bool IsValidManagedPluginId(std::string const& value) noexcept {
	if (value.empty() || value.size() > 128 ||
		!IsAsciiLowerAlphaNumeric(value.front()) ||
		!IsAsciiLowerAlphaNumeric(value.back()))
		return false;
	return std::all_of(value.begin(), value.end(), [](char character) {
		return IsAsciiLowerAlphaNumeric(character) || character == '.' ||
			character == '_' || character == '-';
	});
}

bool IsValidManagedPluginVersion(std::string const& value) noexcept {
	if (value.empty() || value.size() > 128 || !IsAsciiAlphaNumeric(value.front()))
		return false;
	return std::all_of(value.begin(), value.end(), [](char character) {
		return IsAsciiAlphaNumeric(character) || character == '.' ||
			character == '_' || character == '-' || character == '+';
	});
}

ManagedPluginActivationStore::ManagedPluginActivationStore(agi::fs::path root)
: root(std::move(root)) {
	if (this->root.empty())
		throw std::invalid_argument("Managed plugin activation root cannot be empty");
}

agi::fs::path ManagedPluginActivationStore::PluginsDirectory() const {
	return root / "plugins";
}

agi::fs::path ManagedPluginActivationStore::StagingDirectory() const {
	return root / "staging";
}

agi::fs::path ManagedPluginActivationStore::PluginDirectory(
	std::string const& plugin_id) const {
	if (!IsValidManagedPluginId(plugin_id))
		throw std::invalid_argument("Invalid managed plugin ID");
	return PluginsDirectory() / agi::fs::PathFromString(plugin_id);
}

agi::fs::path ManagedPluginActivationStore::VersionDirectory(
	std::string const& plugin_id,
	std::string const& version) const {
	if (!IsValidManagedPluginVersion(version))
		throw std::invalid_argument("Invalid managed plugin version");
	return PluginDirectory(plugin_id) / "versions" /
		agi::fs::PathFromString(version);
}

agi::fs::path ManagedPluginActivationStore::ManifestPath(
	std::string const& plugin_id,
	std::string const& version) const {
	return VersionDirectory(plugin_id, version) / kManagedPluginManifestFilename;
}

agi::fs::path ManagedPluginActivationStore::ActivationPath(
	std::string const& plugin_id) const {
	return PluginDirectory(plugin_id) / "activation.json";
}

agi::fs::path ManagedPluginActivationStore::CreateStagingDirectory(
	std::string const& plugin_id,
	std::string const& version) const {
	if (!IsValidManagedPluginId(plugin_id))
		throw std::invalid_argument("Invalid managed plugin ID");
	if (!IsValidManagedPluginVersion(version))
		throw std::invalid_argument("Invalid managed plugin version");
	agi::fs::CreateDirectory(StagingDirectory());
	auto path = agi::fs::UniquePath(
		StagingDirectory() /
		agi::fs::PathFromString(plugin_id + "-" + version + "-%%%%%%%%"));
	agi::fs::CreateDirectory(path);
	return path;
}

agi::fs::path ManagedPluginActivationStore::PublishStagedVersion(
	std::string const& plugin_id,
	std::string const& version,
	agi::fs::path const& staged_payload_directory) const {
	if (!agi::fs::DirectoryExists(staged_payload_directory))
		throw std::runtime_error("Managed plugin staged payload directory does not exist");
	RejectLinkLikeEntry(
		staged_payload_directory, "Managed plugin staged payload directory");

	auto canonical_stage = agi::fs::Canonicalize(staged_payload_directory);
	auto canonical_staging_root = agi::fs::Canonicalize(StagingDirectory());
	if (!IsWithinDirectory(canonical_stage, canonical_staging_root))
		throw std::runtime_error(
			"Managed plugin payload must be published from this store's staging directory");
	RejectLinkedStagedEntries(canonical_stage);
	ValidateManifest(
		canonical_stage / kManagedPluginManifestFilename, plugin_id, version);

	auto target = VersionDirectory(plugin_id, version);
	EnsurePlainDirectory(
		PluginDirectory(plugin_id), "Managed plugin directory");
	EnsurePlainDirectory(
		target.parent_path(), "Managed plugin versions directory");
	auto canonical_plugins_root = agi::fs::Canonicalize(PluginsDirectory());
	auto canonical_target_parent = agi::fs::Canonicalize(target.parent_path());
	if (!IsWithinDirectory(canonical_target_parent, canonical_plugins_root))
		throw std::runtime_error(
			"Managed plugin version target escapes this store's plugins directory");
	if (agi::fs::Exists(target))
		throw std::runtime_error("Managed plugin version is already installed");
	agi::fs::Rename(canonical_stage, target);
	return target;
}

ManagedPluginActivationState ManagedPluginActivationStore::ReadState(
	std::string const& plugin_id) const {
	auto state = ParseState(ParseObjectFile(
		ActivationPath(plugin_id), "Managed plugin activation"));
	if (state.plugin_id != plugin_id)
		throw std::runtime_error(
			"Managed plugin activation ID does not match its directory");
	return state;
}

std::optional<ManagedPluginActivationState>
ManagedPluginActivationStore::TryReadState(std::string const& plugin_id) const {
	auto path = ActivationPath(plugin_id);
	if (!agi::fs::FileExists(path)) return std::nullopt;
	return ReadState(plugin_id);
}

ManagedPluginActivationState ManagedPluginActivationStore::Activate(
	std::string const& plugin_id,
	std::string const& version) {
	auto manifest = ManifestPath(plugin_id, version);
	ValidateInstalledManifest(manifest, PluginsDirectory(), plugin_id, version);

	auto existing = TryReadState(plugin_id);
	ManagedPluginActivationState state;
	state.plugin_id = plugin_id;
	state.enabled = true;
	state.active_version = version;
	state.generation = 1;
	if (existing) {
		state = *existing;
		if (state.pinned_version && *state.pinned_version != version)
			throw std::runtime_error(
				"Managed plugin is pinned to version '" + *state.pinned_version + "'");
		if (state.active_version != version)
			state.previous_version = state.active_version;
		state.enabled = true;
		state.active_version = version;
		state.last_failed_version.reset();
		state.generation = NextGeneration(state.generation);
	}
	WriteState(ActivationPath(plugin_id), state);
	return state;
}

ManagedPluginActivationState ManagedPluginActivationStore::SetPinnedVersion(
	std::string const& plugin_id,
	std::optional<std::string> version) {
	auto state = ReadState(plugin_id);
	if (version) {
		if (!IsValidManagedPluginVersion(*version))
			throw std::invalid_argument("Invalid managed plugin pinned version");
		if (*version != state.active_version)
			throw std::runtime_error(
				"Only the active managed plugin version can be pinned");
		ValidateInstalledManifest(
			ManifestPath(plugin_id, *version),
			PluginsDirectory(),
			plugin_id,
			*version);
	}
	if (state.pinned_version == version) return state;
	state.pinned_version = std::move(version);
	state.generation = NextGeneration(state.generation);
	WriteState(ActivationPath(plugin_id), state);
	return state;
}

ManagedPluginActivationState ManagedPluginActivationStore::SetEnabled(
	std::string const& plugin_id,
	bool enabled) {
	auto state = ReadState(plugin_id);
	if (enabled)
		ValidateInstalledManifest(
			ManifestPath(plugin_id, state.active_version),
			PluginsDirectory(),
			plugin_id,
			state.active_version);
	if (state.enabled == enabled) return state;
	state.enabled = enabled;
	state.generation = NextGeneration(state.generation);
	WriteState(ActivationPath(plugin_id), state);
	return state;
}

ManagedPluginActivationState ManagedPluginActivationStore::Rollback(
	std::string const& plugin_id) {
	auto state = ReadState(plugin_id);
	if (!state.previous_version)
		throw std::runtime_error("Managed plugin has no previous version to roll back to");
	if (state.pinned_version && *state.pinned_version != *state.previous_version)
		throw std::runtime_error(
			"Managed plugin is pinned to version '" + *state.pinned_version + "'");
	ValidateInstalledManifest(
		ManifestPath(plugin_id, *state.previous_version),
		PluginsDirectory(),
		plugin_id,
		*state.previous_version);
	auto old_active = state.active_version;
	state.active_version = *state.previous_version;
	state.previous_version = std::move(old_active);
	state.last_failed_version.reset();
	state.enabled = true;
	state.generation = NextGeneration(state.generation);
	WriteState(ActivationPath(plugin_id), state);
	return state;
}

ManagedPluginDiscoveryResult ManagedPluginActivationStore::Discover() const {
	ManagedPluginDiscoveryResult result;
	if (!agi::fs::DirectoryExists(PluginsDirectory())) return result;

	try {
		for (auto entry : agi::fs::DirectoryIterator(PluginsDirectory(), "*")) {
			std::string plugin_id;
			try {
				plugin_id = agi::fs::PathToString(
					agi::fs::PathFromString(entry).filename());
				if (!IsValidManagedPluginId(plugin_id)) {
					result.diagnostics.push_back({
						{},
						"Ignoring managed plugin directory with an invalid ID",
						true});
					continue;
				}
				auto const plugin_directory = PluginDirectory(plugin_id);
				if (!agi::fs::DirectoryExists(plugin_directory)) continue;
				RejectLinkLikeEntry(
					plugin_directory, "Managed plugin directory");

				auto state = ReadState(plugin_id);
				if (!state.enabled) continue;

				ManagedPluginLoadCandidate candidate;
				candidate.plugin_id = plugin_id;
				candidate.version = state.active_version;
				candidate.manifest_path = ManifestPath(plugin_id, state.active_version);
				candidate.generation = state.generation;

				try {
					ValidateInstalledManifest(
						candidate.manifest_path,
						PluginsDirectory(),
						plugin_id,
						state.active_version);
				}
				catch (...) {
					auto active_error = DescribeException();
					if (!state.previous_version) throw;
					auto fallback_path = ManifestPath(plugin_id, *state.previous_version);
					ValidateInstalledManifest(
						fallback_path,
						PluginsDirectory(),
						plugin_id,
						*state.previous_version);
					candidate.rollback_from_version = state.active_version;
					candidate.version = *state.previous_version;
					candidate.manifest_path = std::move(fallback_path);
					result.diagnostics.push_back({
						plugin_id,
						"Active version '" + state.active_version +
						"' is invalid (" + active_error +
						"); attempting previous version '" + candidate.version + "'",
						false});
				}

				if (!candidate.rollback_from_version && state.previous_version) {
					auto fallback_path = ManifestPath(plugin_id, *state.previous_version);
					try {
						ValidateInstalledManifest(
							fallback_path,
							PluginsDirectory(),
							plugin_id,
							*state.previous_version);
						candidate.fallback = ManagedPluginFallback{
							*state.previous_version, std::move(fallback_path)};
					}
					catch (...) {
						result.diagnostics.push_back({
							plugin_id,
							"Previous version '" + *state.previous_version +
							"' is not available for rollback: " + DescribeException(),
							false});
					}
				}
				result.candidates.emplace_back(std::move(candidate));
			}
			catch (...) {
				result.diagnostics.push_back({plugin_id, DescribeException(), true});
			}
		}
	}
	catch (...) {
		result.diagnostics.push_back({
			{}, "Could not enumerate the managed plugin store: " + DescribeException(), true});
	}
	return result;
}

bool ManagedPluginActivationStore::CommitAutomaticRollback(
	std::string const& plugin_id,
	uint64_t expected_generation,
	std::string const& failed_version,
	std::string const& fallback_version) {
	auto state = ReadState(plugin_id);
	if (state.generation != expected_generation ||
		state.active_version != failed_version ||
		!state.previous_version || *state.previous_version != fallback_version)
		return false;
	ValidateInstalledManifest(
		ManifestPath(plugin_id, fallback_version),
		PluginsDirectory(),
		plugin_id,
		fallback_version);
	state.active_version = fallback_version;
	state.previous_version.reset();
	state.last_failed_version = failed_version;
	state.pinned_version.reset();
	state.enabled = true;
	state.generation = NextGeneration(state.generation);
	WriteState(ActivationPath(plugin_id), state);
	return true;
}

} // namespace agi::coreclr
