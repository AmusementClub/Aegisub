#include <main.h>

#include "../../src/coreclr/managed_plugin_activation.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <filesystem>
#include <string>

using namespace agi::coreclr;

namespace {

class managed_plugin_activation : public ::testing::Test {
protected:
	agi::fs::path root = agi::fs::UniquePath(
		agi::fs::PathFromString("data/managed_plugin_activation_%%%%%%%%"));
	ManagedPluginActivationStore store{root};

	void SetUp() override {
		agi::fs::CreateDirectory(root);
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}

	void WriteManifest(
		std::string const& plugin_id,
		std::string const& version,
		std::string manifest_id = {},
		std::string manifest_version = {},
		std::string runtime_kind = "coreclr") {
		auto directory = store.VersionDirectory(plugin_id, version);
		agi::fs::CreateDirectory(directory);
		agi::io::Save output(store.ManifestPath(plugin_id, version));
		output.Get()
			<< "{\"manifestVersion\":2,\"id\":\""
			<< (manifest_id.empty() ? plugin_id : manifest_id)
			<< "\",\"version\":\""
			<< (manifest_version.empty() ? version : manifest_version)
			<< "\",\"runtime\":{\"kind\":\"" << runtime_kind << "\"}}";
		output.Close();
	}

	void WriteManifestAt(
		agi::fs::path const& directory,
		std::string const& plugin_id,
		std::string const& version) {
		agi::fs::CreateDirectory(directory);
		agi::io::Save output(directory / kManagedPluginManifestFilename);
		output.Get()
			<< "{\"manifestVersion\":2,\"id\":\"" << plugin_id
			<< "\",\"version\":\"" << version
			<< "\",\"runtime\":{\"kind\":\"coreclr\"}}";
		output.Close();
	}
};

} // namespace

TEST(managed_plugin_activation_identifiers, accepts_safe_ids_and_versions) {
	EXPECT_TRUE(IsValidManagedPluginId("sample.plugin-1"));
	EXPECT_FALSE(IsValidManagedPluginId("Sample.Plugin"));
	EXPECT_FALSE(IsValidManagedPluginId("../plugin"));
	EXPECT_FALSE(IsValidManagedPluginId("plugin/child"));
	EXPECT_TRUE(IsValidManagedPluginVersion("1.2.3-beta.1+win-x64"));
	EXPECT_FALSE(IsValidManagedPluginVersion("../1.0.0"));
	EXPECT_FALSE(IsValidManagedPluginVersion("1.0/child"));
}

TEST_F(managed_plugin_activation, switches_versions_and_supports_manual_rollback) {
	WriteManifest("sample.plugin", "1.0.0");
	auto first = store.Activate("sample.plugin", "1.0.0");
	EXPECT_EQ(first.generation, 1U);
	EXPECT_EQ(first.active_version, "1.0.0");
	EXPECT_FALSE(first.previous_version.has_value());

	WriteManifest("sample.plugin", "2.0.0");
	EXPECT_THROW(
		store.SetPinnedVersion("sample.plugin", "2.0.0"),
		std::runtime_error);
	auto second = store.Activate("sample.plugin", "2.0.0");
	EXPECT_EQ(second.generation, 2U);
	ASSERT_TRUE(second.previous_version.has_value());
	EXPECT_EQ(*second.previous_version, "1.0.0");
	store.SetPinnedVersion("sample.plugin", "2.0.0");
	EXPECT_THROW(store.Activate("sample.plugin", "1.0.0"), std::runtime_error);
	EXPECT_THROW(store.Rollback("sample.plugin"), std::runtime_error);
	store.SetPinnedVersion("sample.plugin", std::nullopt);

	auto discovery = store.Discover();
	ASSERT_EQ(discovery.candidates.size(), 1U);
	EXPECT_EQ(discovery.candidates[0].version, "2.0.0");
	ASSERT_TRUE(discovery.candidates[0].fallback.has_value());
	EXPECT_EQ(discovery.candidates[0].fallback->version, "1.0.0");

	auto rolled_back = store.Rollback("sample.plugin");
	EXPECT_EQ(rolled_back.active_version, "1.0.0");
	ASSERT_TRUE(rolled_back.previous_version.has_value());
	EXPECT_EQ(*rolled_back.previous_version, "2.0.0");

	store.SetEnabled("sample.plugin", false);
	EXPECT_TRUE(store.Discover().candidates.empty());
	EXPECT_FALSE(store.ReadState("sample.plugin").enabled);
	store.SetEnabled("sample.plugin", true);
	EXPECT_EQ(store.Discover().candidates.size(), 1U);
}

TEST_F(managed_plugin_activation, invalid_active_version_falls_back_and_commits_atomically) {
	WriteManifest("sample.plugin", "1.0.0");
	store.Activate("sample.plugin", "1.0.0");
	WriteManifest("sample.plugin", "2.0.0");
	auto activated = store.Activate("sample.plugin", "2.0.0");
	ASSERT_TRUE(agi::fs::Remove(store.ManifestPath("sample.plugin", "2.0.0")));

	auto discovery = store.Discover();
	ASSERT_EQ(discovery.candidates.size(), 1U);
	auto const& candidate = discovery.candidates[0];
	EXPECT_EQ(candidate.version, "1.0.0");
	ASSERT_TRUE(candidate.rollback_from_version.has_value());
	EXPECT_EQ(*candidate.rollback_from_version, "2.0.0");
	EXPECT_TRUE(store.CommitAutomaticRollback(
		"sample.plugin",
		activated.generation,
		"2.0.0",
		"1.0.0"));

	auto state = store.ReadState("sample.plugin");
	EXPECT_EQ(state.active_version, "1.0.0");
	EXPECT_FALSE(state.previous_version.has_value());
	ASSERT_TRUE(state.last_failed_version.has_value());
	EXPECT_EQ(*state.last_failed_version, "2.0.0");
	EXPECT_FALSE(store.CommitAutomaticRollback(
		"sample.plugin",
		activated.generation,
		"2.0.0",
		"1.0.0"));
}

TEST_F(managed_plugin_activation, rejects_mismatched_or_unsafe_activation) {
	WriteManifest("sample.plugin", "1.0.0", "different.plugin", "1.0.0");
	EXPECT_THROW(store.Activate("sample.plugin", "1.0.0"), std::runtime_error);
	EXPECT_THROW(store.Activate("../sample", "1.0.0"), std::invalid_argument);
	EXPECT_THROW(store.Activate("sample.plugin", "../1.0.0"), std::invalid_argument);

	WriteManifest("sample.plugin", "2.0.0", "sample.plugin", "1.0.0");
	EXPECT_THROW(store.Activate("sample.plugin", "2.0.0"), std::runtime_error);
}

TEST_F(managed_plugin_activation, accepts_native_and_rejects_legacy_nativeaot_kind) {
	WriteManifest("native.plugin", "1.0.0", {}, {}, "native");
	EXPECT_NO_THROW(store.Activate("native.plugin", "1.0.0"));

	WriteManifest("legacy.plugin", "1.0.0", {}, {}, "nativeaot");
	EXPECT_THROW(store.Activate("legacy.plugin", "1.0.0"), std::runtime_error);
}

TEST_F(managed_plugin_activation, corrupt_state_is_diagnostic_not_process_failure) {
	auto directory = store.PluginDirectory("sample.plugin");
	agi::fs::CreateDirectory(directory);
	agi::io::Save output(store.ActivationPath("sample.plugin"));
	output.Get() << "{broken";
	output.Close();

	auto discovery = store.Discover();
	EXPECT_TRUE(discovery.candidates.empty());
	ASSERT_EQ(discovery.diagnostics.size(), 1U);
	EXPECT_EQ(discovery.diagnostics[0].plugin_id, "sample.plugin");
	EXPECT_TRUE(discovery.diagnostics[0].error);
}

TEST_F(managed_plugin_activation, publishes_only_valid_same_store_staging_payloads) {
	auto staged = store.CreateStagingDirectory("sample.plugin", "1.0.0");
	WriteManifestAt(staged, "sample.plugin", "1.0.0");
	auto published = store.PublishStagedVersion(
		"sample.plugin", "1.0.0", staged);
	EXPECT_EQ(published, store.VersionDirectory("sample.plugin", "1.0.0"));
	EXPECT_FALSE(agi::fs::Exists(staged));
	EXPECT_TRUE(agi::fs::FileExists(
		published / kManagedPluginManifestFilename));
	EXPECT_NO_THROW(store.Activate("sample.plugin", "1.0.0"));

	auto duplicate = store.CreateStagingDirectory("sample.plugin", "1.0.0");
	WriteManifestAt(duplicate, "sample.plugin", "1.0.0");
	EXPECT_THROW(
		store.PublishStagedVersion("sample.plugin", "1.0.0", duplicate),
		std::runtime_error);

	auto outside = root / "outside";
	WriteManifestAt(outside, "other.plugin", "1.0.0");
	EXPECT_THROW(
		store.PublishStagedVersion("other.plugin", "1.0.0", outside),
		std::runtime_error);

	auto linked = store.CreateStagingDirectory("linked.plugin", "1.0.0");
	WriteManifestAt(linked, "linked.plugin", "1.0.0");
	std::error_code link_error;
	std::filesystem::create_directory_symlink(
		outside, linked / "linked-payload", link_error);
	if (!link_error)
		EXPECT_THROW(
			store.PublishStagedVersion("linked.plugin", "1.0.0", linked),
			std::runtime_error);
}

TEST(managed_plugin_activation_unicode, supports_unicode_store_roots) {
	auto root = agi::fs::UniquePath(agi::fs::PathFromString(
		"data/managed-plugin-\xE6\xB5\x8B\xE8\xAF\x95-\xF0\x9F\x98\x80-%%%%%%%%"));
	std::error_code error;
	std::filesystem::remove_all(root, error);
	agi::fs::CreateDirectory(root);
	try {
		ManagedPluginActivationStore store(root);
		auto directory = store.VersionDirectory("sample.plugin", "1.0.0");
		agi::fs::CreateDirectory(directory);
		agi::io::Save output(store.ManifestPath("sample.plugin", "1.0.0"));
		output.Get() << R"({"manifestVersion":2,"id":"sample.plugin","version":"1.0.0","runtime":{"kind":"coreclr"}})";
		output.Close();
		EXPECT_NO_THROW(store.Activate("sample.plugin", "1.0.0"));
		EXPECT_EQ(store.Discover().candidates.size(), 1U);
	}
	catch (...) {
		std::filesystem::remove_all(root, error);
		throw;
	}
	std::filesystem::remove_all(root, error);
}
