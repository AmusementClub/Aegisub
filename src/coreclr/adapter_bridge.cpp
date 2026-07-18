#include "adapter_bridge.h"

#include "native_library.h"

#include <libaegisub/fs.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace agi::coreclr {
namespace {

std::string StatusMessage(
	std::string operation,
	int32_t status,
	std::optional<BridgeError> const& error,
	std::string const& raw_error) {
	auto message = std::move(operation) + " failed with Bridge status " + std::to_string(status);
	if (error)
		message += " [" + error->code + "]: " + error->message;
	else if (!raw_error.empty())
		message += ": " + raw_error;
	return message;
}

int32_t CheckedLength(std::string const& value, char const* description) {
	if (value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
		throw std::length_error(std::string(description) + " exceeds the Bridge ABI length limit");
	return static_cast<int32_t>(value.size());
}

template<typename Callback, typename StatusCallback>
std::string ReadUtf8Payload(
	std::string const& operation,
	Callback&& callback,
	StatusCallback&& status_callback) {
	int32_t payload_length = 0;
	auto status = callback(nullptr, 0, &payload_length);
	if (status != static_cast<int32_t>(BridgeStatus::BufferTooSmall) || payload_length < 0)
		status_callback(operation + " size query", status);
	if (payload_length == std::numeric_limits<int32_t>::max())
		throw std::length_error(operation + " exceeds the Bridge ABI buffer limit");

	std::vector<uint8_t> buffer(static_cast<size_t>(payload_length) + 1);
	status = callback(buffer.data(), static_cast<int32_t>(buffer.size()), &payload_length);
	if (status != static_cast<int32_t>(BridgeStatus::Success))
		status_callback(operation, status);
	if (payload_length < 0 || static_cast<size_t>(payload_length) >= buffer.size())
		throw std::runtime_error(operation + " returned an invalid payload length");

	return std::string(reinterpret_cast<char const*>(buffer.data()), static_cast<size_t>(payload_length));
}

} // namespace

BridgeException::BridgeException(
	std::string operation,
	int32_t status,
	std::optional<BridgeError> error,
	std::string raw_error)
: std::runtime_error(StatusMessage(operation, status, error, raw_error))
, status(status)
, error(std::move(error))
, raw_error(std::move(raw_error)) {
}

AdapterBridge::AdapterBridge(AdapterBridgeOptions options)
: runtime(std::make_unique<Host>(std::move(options.runtime))) {
	if (options.native_api.abi_version != kNativeBridgeAbiVersion ||
		options.native_api.size != sizeof(NativeHostApi))
		throw std::invalid_argument("NativeHostApi has an incompatible ABI version or size");

	auto initialize = reinterpret_cast<InitializeAdapterFn>(
		runtime->LoadUnmanagedEntryPoint(
			options.adapter_assembly_path,
			"Aegisub.CoreClr.Adapter.BridgeEntryPoints, Aegisub.CoreClr.Adapter",
			"Initialize"));

	auto status = initialize(&options.native_api, &adapter_api);
	if (status != static_cast<int32_t>(BridgeStatus::Success))
		throw std::runtime_error(
			"CoreCLR Adapter Bridge initialization failed with status " + std::to_string(status));
	if (adapter_api.abi_version != kNativeBridgeAbiVersion ||
		adapter_api.size != sizeof(AdapterApi) ||
		!adapter_api.get_runtime_info_utf8 || !adapter_api.load_plugin_utf8 ||
		!adapter_api.get_plugin_metadata_utf8 || !adapter_api.invoke_contribution_utf8 ||
		!adapter_api.dispatch_plugin_event_utf8 || !adapter_api.unload_plugin ||
		!adapter_api.get_last_error_utf8 ||
		!adapter_api.shutdown) {
		if (adapter_api.shutdown)
			adapter_api.shutdown();
		throw std::runtime_error("AdapterApi has an incompatible ABI version, size, or function table");
	}
	initialized = true;
}

AdapterBridge::AdapterBridge(NativeAdapterBridgeOptions options)
: native_library(std::make_unique<NativeLibrary>())
, native_plugin_handle(options.plugin_handle) {
	if (options.native_api.abi_version != kNativeBridgeAbiVersion ||
		options.native_api.size != sizeof(NativeHostApi))
		throw std::invalid_argument("NativeHostApi has an incompatible ABI version or size");
	if (!native_plugin_handle)
		throw std::invalid_argument("Native C ABI plugin handle is zero");
	if (options.library_path.empty())
		throw std::invalid_argument("Native C ABI plugin library path is empty");

	native_library->Load(std::filesystem::absolute(options.library_path));
	try {
		auto initialize = native_library->Resolve<InitializeNativePluginFn>(
			kNativePluginEntryPoint);
		auto status = initialize(
			&options.native_api, native_plugin_handle, &adapter_api);
		if (status != static_cast<int32_t>(BridgeStatus::Success))
			throw std::runtime_error(
				"Native C ABI plugin initialization failed with Bridge status " +
				std::to_string(status));
		if (adapter_api.abi_version != kNativeBridgeAbiVersion ||
			adapter_api.size != sizeof(AdapterApi) ||
			!adapter_api.get_runtime_info_utf8 || adapter_api.load_plugin_utf8 ||
			!adapter_api.get_plugin_metadata_utf8 ||
			!adapter_api.invoke_contribution_utf8 ||
			!adapter_api.dispatch_plugin_event_utf8 || !adapter_api.unload_plugin ||
			!adapter_api.get_last_error_utf8 || !adapter_api.shutdown)
			throw std::runtime_error(
				"Native C ABI plugin has an incompatible ABI version, size, or function table");
		initialized = true;
	}
	catch (...) {
		// A native plugin may own process-lifetime runtime state. It must remain
		// resident even when initialization or ABI validation fails.
		native_library->Detach();
		throw;
	}
}

AdapterBridge::~AdapterBridge() {
	try {
		Shutdown();
	}
	catch (...) {
		// Destructors cannot surface Adapter shutdown failures. Callers which
		// need verification (including the smoke test) use Shutdown explicitly.
	}
}

std::string AdapterBridge::GetRuntimeInfoUtf8() const {
	if (!initialized || !adapter_api.get_runtime_info_utf8)
		throw std::logic_error("Adapter Bridge is not initialized");

	return ReadUtf8Payload(
		"Adapter runtime info query",
		[this](uint8_t* buffer, int32_t capacity, int32_t* length) {
			return adapter_api.get_runtime_info_utf8(buffer, capacity, length);
		},
		[this](std::string operation, int32_t status) {
			ThrowStatus(std::move(operation), status);
		});
}

LoadedPlugin AdapterBridge::LoadPlugin(
	std::filesystem::path const& entry_assembly_path,
	std::string const& entry_type) {
	if (!initialized || !adapter_api.load_plugin_utf8)
		throw std::logic_error("Plugin Bridge is not initialized");
	if (entry_assembly_path.empty())
		throw std::invalid_argument("C# extension entry assembly path is empty");
	if (entry_type.empty())
		throw std::invalid_argument("C# extension entry type is empty");

	auto assembly_path = agi::fs::PathToString(entry_assembly_path);
	uint64_t handle = 0;
	auto status = adapter_api.load_plugin_utf8(
		reinterpret_cast<uint8_t const*>(assembly_path.data()),
		CheckedLength(assembly_path, "C# extension entry assembly path"),
		reinterpret_cast<uint8_t const*>(entry_type.data()),
		CheckedLength(entry_type, "C# extension entry type"),
		&handle);
	if (status != static_cast<int32_t>(BridgeStatus::Success))
		ThrowStatus("C# extension load", status);
	if (handle == 0)
		throw std::runtime_error("CoreCLR Adapter returned an invalid zero extension handle");

	try {
		return {handle, GetPluginMetadataUtf8(handle)};
	}
	catch (...) {
		adapter_api.unload_plugin(handle);
		throw;
	}
}

LoadedPlugin AdapterBridge::LoadNativePlugin(uint64_t plugin_handle) const {
	if (!initialized || !native_library)
		throw std::logic_error("Native C ABI plugin Bridge is not initialized");
	if (!plugin_handle || plugin_handle != native_plugin_handle)
		throw std::invalid_argument("Native C ABI plugin handle does not match its runtime");
	return {plugin_handle, GetPluginMetadataUtf8(plugin_handle)};
}

std::string AdapterBridge::GetPluginMetadataUtf8(uint64_t plugin_handle) const {
	return ReadUtf8Payload(
		"plugin metadata query",
		[this, plugin_handle](uint8_t* buffer, int32_t capacity, int32_t* length) {
			return adapter_api.get_plugin_metadata_utf8(
				plugin_handle, buffer, capacity, length);
		},
		[this](std::string operation, int32_t status) {
			ThrowStatus(std::move(operation), status);
		});
}

std::string AdapterBridge::InvokeContributionUtf8(
	uint64_t plugin_handle,
	std::string const& contribution_id,
	std::string const& operation_id,
	std::string const& request_json) const {
	if (!initialized || !adapter_api.invoke_contribution_utf8)
		throw std::logic_error("Adapter Bridge is not initialized");
	if (plugin_handle == 0)
		throw std::invalid_argument("plugin handle is zero");
	if (operation_id.empty())
		throw std::invalid_argument("plugin operation ID is empty");
	if (request_json.empty())
		throw std::invalid_argument("plugin request JSON is empty");

	auto const* contribution_bytes = contribution_id.empty()
		? nullptr
		: reinterpret_cast<uint8_t const*>(contribution_id.data());
	auto contribution_length = CheckedLength(contribution_id, "plugin contribution ID");
	auto const* operation_bytes = reinterpret_cast<uint8_t const*>(operation_id.data());
	auto operation_length = CheckedLength(operation_id, "plugin operation ID");
	auto const* request_bytes = reinterpret_cast<uint8_t const*>(request_json.data());
	auto request_length = CheckedLength(request_json, "plugin request JSON");

	return ReadUtf8Payload(
		"plugin contribution invocation",
		[this, plugin_handle, contribution_bytes, contribution_length,
			operation_bytes, operation_length, request_bytes, request_length](
				uint8_t* buffer, int32_t capacity, int32_t* payload_length) {
			return adapter_api.invoke_contribution_utf8(
				plugin_handle,
				contribution_bytes,
				contribution_length,
				operation_bytes,
				operation_length,
				request_bytes,
				request_length,
				buffer,
				capacity,
				payload_length);
		},
		[this](std::string operation, int32_t status) {
			ThrowStatus(std::move(operation), status);
		});
}

std::string AdapterBridge::InvokeMacroUtf8(
	uint64_t plugin_handle,
	std::string const& macro_id,
	std::string const& context_json) const {
	if (macro_id.empty())
		throw std::invalid_argument("C# Macro ID is empty");
	return InvokeContributionUtf8(plugin_handle, {}, macro_id, context_json);
}

void AdapterBridge::DispatchPluginEventUtf8(
	uint64_t plugin_handle,
	std::string const& event_id,
	std::string const& payload_json) const {
	if (!initialized || !adapter_api.dispatch_plugin_event_utf8)
		throw std::logic_error("Adapter Bridge is not initialized");
	if (plugin_handle == 0)
		throw std::invalid_argument("plugin handle is zero");
	if (event_id.empty())
		throw std::invalid_argument("plugin event ID is empty");
	if (payload_json.empty())
		throw std::invalid_argument("plugin event payload JSON is empty");

	auto status = adapter_api.dispatch_plugin_event_utf8(
		plugin_handle,
		reinterpret_cast<uint8_t const*>(event_id.data()),
		CheckedLength(event_id, "plugin event ID"),
		reinterpret_cast<uint8_t const*>(payload_json.data()),
		CheckedLength(payload_json, "plugin event payload JSON"));
	if (status != static_cast<int32_t>(BridgeStatus::Success))
		ThrowStatus("plugin event dispatch", status);
}

void AdapterBridge::UnloadPlugin(uint64_t plugin_handle) {
	if (!initialized || !adapter_api.unload_plugin)
		throw std::logic_error("Adapter Bridge is not initialized");
	if (plugin_handle == 0)
		throw std::invalid_argument("plugin handle is zero");

	auto status = adapter_api.unload_plugin(plugin_handle);
	if (status != static_cast<int32_t>(BridgeStatus::Success))
		ThrowStatus("plugin unload", status);
}

std::string AdapterBridge::GetLastErrorUtf8() const noexcept {
	if (!adapter_api.get_last_error_utf8) return {};
	try {
		int32_t payload_length = 0;
		auto status = adapter_api.get_last_error_utf8(nullptr, 0, &payload_length);
		if (status != static_cast<int32_t>(BridgeStatus::BufferTooSmall) || payload_length < 0)
			return {};
		if (payload_length == std::numeric_limits<int32_t>::max()) return {};
		std::vector<uint8_t> buffer(static_cast<size_t>(payload_length) + 1);
		status = adapter_api.get_last_error_utf8(
			buffer.data(), static_cast<int32_t>(buffer.size()), &payload_length);
		if (status != static_cast<int32_t>(BridgeStatus::Success) || payload_length < 0 ||
			static_cast<size_t>(payload_length) >= buffer.size())
			return {};
		return std::string(
			reinterpret_cast<char const*>(buffer.data()), static_cast<size_t>(payload_length));
	}
	catch (...) {
		return {};
	}
}

[[noreturn]] void AdapterBridge::ThrowStatus(std::string operation, int32_t status) const {
	auto raw_error = GetLastErrorUtf8();
	auto error = ParseBridgeErrorEnvelope(raw_error);
	if (status == static_cast<int32_t>(BridgeStatus::Cancelled))
		throw InvocationCancelled(
			std::move(operation), status, std::move(error), std::move(raw_error));
	throw BridgeException(
		std::move(operation), status, std::move(error), std::move(raw_error));
}

void AdapterBridge::Shutdown() {
	if (!initialized) return;
	auto status = adapter_api.shutdown
		? adapter_api.shutdown()
		: static_cast<int32_t>(BridgeStatus::NotInitialized);
	std::string raw_error;
	std::optional<BridgeError> error;
	if (status != static_cast<int32_t>(BridgeStatus::Success)) {
		raw_error = GetLastErrorUtf8();
		error = ParseBridgeErrorEnvelope(raw_error);
	}
	initialized = false;
	adapter_api = {};
	if (native_library) native_library->Detach();
	if (status != static_cast<int32_t>(BridgeStatus::Success))
		throw BridgeException(
			"Adapter Bridge shutdown", status, std::move(error), std::move(raw_error));
}

std::filesystem::path const& AdapterBridge::GetHostFxrPath() const {
	if (!runtime)
		throw std::logic_error("Native C ABI plugins do not use hostfxr");
	return runtime->GetHostFxrPath();
}

} // namespace agi::coreclr
