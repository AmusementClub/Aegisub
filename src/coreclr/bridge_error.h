#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace agi::coreclr {

enum class BridgeErrorCategory {
	Unknown,
	Bridge,
	Runtime,
	Extension,
	Contract,
	Host,
	Invocation
};

struct BridgeError {
	int schema_version = 0;
	std::string code;
	BridgeErrorCategory category = BridgeErrorCategory::Unknown;
	std::string message;
	std::string details;
	std::string exception_type;
	bool retryable = false;
};

std::optional<BridgeError> ParseBridgeErrorEnvelope(std::string_view json);
std::string_view ToString(BridgeErrorCategory category) noexcept;

} // namespace agi::coreclr
