#include <main.h>

#include "../../src/coreclr/plugin_service_registry.h"

#include <stdexcept>
#include <string>
#include <vector>

using Automation4::PluginServiceProviderRegistry;

TEST(plugin_service_registry, retains_provider_until_every_same_extension_owner_releases) {
	PluginServiceProviderRegistry registry;
	std::vector<std::string> operations{"first", "second"};

	registry.Register("service", "extension", operations);
	registry.Register("service", "extension", operations);
	registry.Unregister("service", "extension");

	auto provider = registry.Find("service");
	ASSERT_TRUE(provider.has_value());
	EXPECT_EQ("extension", provider->extension_key);
	EXPECT_EQ(operations, provider->operations);

	registry.Unregister("service", "extension");
	EXPECT_FALSE(registry.Contains("service"));
}

TEST(plugin_service_registry, rejects_other_extensions_without_disturbing_owner) {
	PluginServiceProviderRegistry registry;
	registry.Register("service", "first-extension", {"operation"});

	EXPECT_THROW(
		registry.Register("service", "second-extension", {"operation"}),
		std::runtime_error);
	registry.Unregister("service", "second-extension");

	auto provider = registry.Find("service");
	ASSERT_TRUE(provider.has_value());
	EXPECT_EQ("first-extension", provider->extension_key);
}

TEST(plugin_service_registry, rejects_conflicting_operations_for_same_extension) {
	PluginServiceProviderRegistry registry;
	registry.Register("service", "extension", {"first"});

	EXPECT_THROW(
		registry.Register("service", "extension", {"second"}),
		std::runtime_error);

	registry.Unregister("service", "extension");
	EXPECT_FALSE(registry.Contains("service"));
}
