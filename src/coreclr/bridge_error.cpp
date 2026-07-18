#include "bridge_error.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>

#include <cstdint>
#include <sstream>
#include <utility>

namespace agi::coreclr {
namespace {

std::optional<std::string> FindString(json::Object const& object, char const* key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<int64_t> FindInt(json::Object const& object, char const* key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	try {
		return static_cast<int64_t>(static_cast<json::Integer const&>(it->second));
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<bool> FindBool(json::Object const& object, char const* key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	try {
		return static_cast<json::Boolean const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

BridgeErrorCategory ParseCategory(std::string const& value) {
	if (value == "bridge") return BridgeErrorCategory::Bridge;
	if (value == "runtime") return BridgeErrorCategory::Runtime;
	if (value == "extension") return BridgeErrorCategory::Extension;
	if (value == "contract") return BridgeErrorCategory::Contract;
	if (value == "host") return BridgeErrorCategory::Host;
	if (value == "invocation") return BridgeErrorCategory::Invocation;
	return BridgeErrorCategory::Unknown;
}

} // namespace

std::optional<BridgeError> ParseBridgeErrorEnvelope(std::string_view value) {
	try {
		std::istringstream stream{std::string(value)};
		json::UnknownElement root;
		json::Reader::Read(root, stream);
		auto const& object = static_cast<json::Object const&>(root);

		auto schema_version = FindInt(object, "schemaVersion");
		auto code = FindString(object, "code");
		auto category_name = FindString(object, "category");
		auto message = FindString(object, "message");
		auto retryable = FindBool(object, "retryable");
		if (!schema_version || *schema_version != 1 || !code || code->empty() ||
			!category_name || !message || message->empty() || !retryable)
			return std::nullopt;

		auto category = ParseCategory(*category_name);
		if (category == BridgeErrorCategory::Unknown)
			return std::nullopt;

		BridgeError error;
		error.schema_version = static_cast<int>(*schema_version);
		error.code = std::move(*code);
		error.category = category;
		error.message = std::move(*message);
		error.details = FindString(object, "details").value_or("");
		error.exception_type = FindString(object, "exceptionType").value_or("");
		error.retryable = *retryable;
		return error;
	}
	catch (...) {
		return std::nullopt;
	}
}

std::string_view ToString(BridgeErrorCategory category) noexcept {
	switch (category) {
		case BridgeErrorCategory::Bridge: return "bridge";
		case BridgeErrorCategory::Runtime: return "runtime";
		case BridgeErrorCategory::Extension: return "extension";
		case BridgeErrorCategory::Contract: return "contract";
		case BridgeErrorCategory::Host: return "host";
		case BridgeErrorCategory::Invocation: return "invocation";
		default: return "unknown";
	}
}

} // namespace agi::coreclr
