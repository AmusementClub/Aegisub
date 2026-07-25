#include "plugin_service_registry.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace Automation4 {

void PluginServiceProviderRegistry::Register(
	std::string const& contribution_id,
	std::string const& extension_key,
	std::vector<std::string> operations) {
	if (contribution_id.empty())
		throw std::invalid_argument("Plugin service contribution ID cannot be empty");
	if (extension_key.empty())
		throw std::invalid_argument("Plugin service registration requires an extension key");

	std::lock_guard<std::mutex> lock(mutex);
	auto existing = providers.find(contribution_id);
	if (existing == providers.end()) {
		providers.emplace(
			contribution_id,
			Entry{
				PluginServiceProviderRegistration{
					extension_key,
					std::move(operations)},
				1});
		return;
	}

	if (existing->second.provider.extension_key != extension_key) {
		throw std::runtime_error(
			"Plugin service contribution '" + contribution_id +
			"' is already registered by another plugin");
	}
	if (existing->second.provider.operations != operations) {
		throw std::runtime_error(
			"Plugin service contribution '" + contribution_id +
			"' has conflicting operation declarations");
	}
	if (existing->second.owner_count == std::numeric_limits<size_t>::max())
		throw std::overflow_error("Plugin service registration count is exhausted");
	++existing->second.owner_count;
}

void PluginServiceProviderRegistry::Unregister(
	std::string const& contribution_id,
	std::string const& extension_key) noexcept {
	std::lock_guard<std::mutex> lock(mutex);
	auto existing = providers.find(contribution_id);
	if (existing == providers.end() ||
		existing->second.provider.extension_key != extension_key)
		return;
	if (existing->second.owner_count > 1) {
		--existing->second.owner_count;
		return;
	}
	providers.erase(existing);
}

bool PluginServiceProviderRegistry::Contains(
	std::string const& contribution_id) const {
	std::lock_guard<std::mutex> lock(mutex);
	return providers.find(contribution_id) != providers.end();
}

std::optional<PluginServiceProviderRegistration>
PluginServiceProviderRegistry::Find(std::string const& contribution_id) const {
	std::lock_guard<std::mutex> lock(mutex);
	auto existing = providers.find(contribution_id);
	if (existing == providers.end())
		return std::nullopt;
	return existing->second.provider;
}

void PluginServiceProviderRegistry::Clear() noexcept {
	std::lock_guard<std::mutex> lock(mutex);
	providers.clear();
}

} // namespace Automation4
