#pragma once

#include "bridge_abi.h"
#include "bridge_error.h"
#include "host.h"

#include <filesystem>
#include <memory>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace agi::coreclr {

class NativeLibrary;

struct AdapterBridgeOptions {
	HostOptions runtime;
	std::filesystem::path adapter_assembly_path;
	NativeHostApi native_api;
};

struct NativeAdapterBridgeOptions {
	std::filesystem::path library_path;
	uint64_t plugin_handle = 0;
	NativeHostApi native_api;
};

struct LoadedPlugin {
	uint64_t handle = 0;
	std::string metadata_json;
};

class BridgeException : public std::runtime_error {
private:
	int32_t status;
	std::optional<BridgeError> error;
	std::string raw_error;

public:
	BridgeException(
		std::string operation,
		int32_t status,
		std::optional<BridgeError> error,
		std::string raw_error);

	int32_t Status() const noexcept { return status; }
	std::optional<BridgeError> const& Error() const noexcept { return error; }
	std::string const& RawError() const noexcept { return raw_error; }
};

class InvocationCancelled final : public BridgeException {
public:
	using BridgeException::BridgeException;
};

class AdapterBridge final {
	std::unique_ptr<Host> runtime;
	std::unique_ptr<NativeLibrary> native_library;
	AdapterApi adapter_api;
	uint64_t native_plugin_handle = 0;
	bool initialized = false;

	std::string GetPluginMetadataUtf8(uint64_t plugin_handle) const;
	std::string GetLastErrorUtf8() const noexcept;
	[[noreturn]] void ThrowStatus(std::string operation, int32_t status) const;

public:
	explicit AdapterBridge(AdapterBridgeOptions options);
	explicit AdapterBridge(NativeAdapterBridgeOptions options);
	~AdapterBridge();

	AdapterBridge(AdapterBridge const&) = delete;
	AdapterBridge& operator=(AdapterBridge const&) = delete;
	AdapterBridge(AdapterBridge&&) = delete;
	AdapterBridge& operator=(AdapterBridge&&) = delete;

	std::string GetRuntimeInfoUtf8() const;
	LoadedPlugin LoadPlugin(
		std::filesystem::path const& entry_assembly_path,
		std::string const& entry_type);
	LoadedPlugin LoadNativePlugin(uint64_t plugin_handle) const;
	std::string InvokeContributionUtf8(
		uint64_t plugin_handle,
		std::string const& contribution_id,
		std::string const& operation_id,
		std::string const& request_json) const;
	// Temporary Automation adapter while Macro remains the only executable
	// public contribution. An empty contribution ID requests unique Macro lookup.
	std::string InvokeMacroUtf8(
		uint64_t plugin_handle,
		std::string const& macro_id,
		std::string const& context_json) const;
	void DispatchPluginEventUtf8(
		uint64_t plugin_handle,
		std::string const& event_id,
		std::string const& payload_json) const;
	void UnloadPlugin(uint64_t plugin_handle);
	/// Explicitly shut down the managed Adapter and report unload failures.
	/// The destructor performs the same operation on a best-effort basis.
	void Shutdown();
	std::filesystem::path const& GetHostFxrPath() const;
};

} // namespace agi::coreclr
