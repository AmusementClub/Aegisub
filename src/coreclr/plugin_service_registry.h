#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Automation4 {

struct PluginServiceProviderRegistration {
	std::string extension_key;
	std::vector<std::string> operations;
};

/// Tracks committed service-provider registrations. Multiple script or lazy
/// bootstrap instances may own the same contribution for one extension; the
/// contribution remains available until every owner releases its registration.
class PluginServiceProviderRegistry final {
	struct Entry {
		PluginServiceProviderRegistration provider;
		size_t owner_count = 0;
	};

	mutable std::mutex mutex;
	std::map<std::string, Entry, std::less<>> providers;

public:
	void Register(
		std::string const& contribution_id,
		std::string const& extension_key,
		std::vector<std::string> operations);
	void Unregister(
		std::string const& contribution_id,
		std::string const& extension_key) noexcept;

	bool Contains(std::string const& contribution_id) const;
	std::optional<PluginServiceProviderRegistration> Find(
		std::string const& contribution_id) const;
	void Clear() noexcept;
};

} // namespace Automation4
