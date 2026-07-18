#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace Automation4 {

std::optional<std::string> InvokeDependencyControlHostService(
	uint64_t plugin_handle,
	std::string const& service_id,
	std::string const& request_json);

void AbortDependencyControlTransactions(uint64_t plugin_handle) noexcept;
void ShutdownDependencyControlHost() noexcept;

} // namespace Automation4
