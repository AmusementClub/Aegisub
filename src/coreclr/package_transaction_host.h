#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace Automation4 {

std::optional<std::string> InvokePackageTransactionHostService(
	uint64_t plugin_handle,
	std::string const& service_id,
	std::string const& request_json);

void AbortPackageTransactions(uint64_t plugin_handle) noexcept;
void ShutdownPackageTransactionHost() noexcept;

} // namespace Automation4
