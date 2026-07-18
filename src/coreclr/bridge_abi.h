#pragma once

#include <cstdint>
#include <type_traits>

#ifdef _WIN32
#define AEGISUB_PLUGIN_BRIDGE_CALL __stdcall
#else
#define AEGISUB_PLUGIN_BRIDGE_CALL
#endif

namespace agi::coreclr {

// Internal exact-match guard. The Native host and managed Adapter are shipped
// together; this is not a public compatibility or package version.
inline constexpr uint32_t kNativeBridgeAbiVersion = 1;

enum class BridgeStatus : int32_t {
	Success = 0,
	InvalidArgument = -1,
	IncompatibleAbi = -2,
	BufferTooSmall = -3,
	AlreadyInitialized = -4,
	NotInitialized = -5,
	NotFound = -6,
	ExtensionLoadFailed = -7,
	InvalidHandle = -8,
	InvocationFailed = -9,
	UnloadFailed = -10,
	Cancelled = -11,
	HostServiceFailed = -12,
	EventDispatchFailed = -13,
	AdapterException = -100
};

using LogUtf8Fn = void(AEGISUB_PLUGIN_BRIDGE_CALL *)(uint8_t const* message, int32_t length);
using IsCancellationRequestedFn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(int64_t invocation_token);
using ReportProgressFn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	int64_t invocation_token,
	int64_t current,
	int64_t maximum,
	uint8_t const* message,
	int32_t message_length);
using InvokeHostServiceUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint64_t plugin_handle,
	int64_t invocation_token,
	uint8_t const* service_id,
	int32_t service_id_length,
	uint8_t const* request_json,
	int32_t request_json_length,
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length);
using EmitPluginEventUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint64_t plugin_handle,
	uint8_t const* event_id,
	int32_t event_id_length,
	uint8_t const* payload_json,
	int32_t payload_json_length);

struct NativeHostApi {
	uint32_t abi_version = kNativeBridgeAbiVersion;
	uint32_t size = sizeof(NativeHostApi);
	LogUtf8Fn log_utf8 = nullptr;
	IsCancellationRequestedFn is_cancellation_requested = nullptr;
	ReportProgressFn report_progress = nullptr;
	InvokeHostServiceUtf8Fn invoke_host_service_utf8 = nullptr;
	EmitPluginEventUtf8Fn emit_plugin_event_utf8 = nullptr;
};

using GetRuntimeInfoUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length);
using LoadPluginUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint8_t const* assembly_path,
	int32_t assembly_path_length,
	uint8_t const* entry_type,
	int32_t entry_type_length,
	uint64_t* plugin_handle);
using GetPluginMetadataUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint64_t plugin_handle,
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length);
using InvokeContributionUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint64_t plugin_handle,
	uint8_t const* contribution_id,
	int32_t contribution_id_length,
	uint8_t const* operation_id,
	int32_t operation_id_length,
	uint8_t const* request_json,
	int32_t request_json_length,
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length);
using DispatchPluginEventUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint64_t plugin_handle,
	uint8_t const* event_id,
	int32_t event_id_length,
	uint8_t const* payload_json,
	int32_t payload_json_length);
using UnloadPluginFn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(uint64_t plugin_handle);
using GetLastErrorUtf8Fn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length);
using ShutdownAdapterFn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)();

struct AdapterApi {
	uint32_t abi_version = 0;
	uint32_t size = 0;
	GetRuntimeInfoUtf8Fn get_runtime_info_utf8 = nullptr;
	LoadPluginUtf8Fn load_plugin_utf8 = nullptr;
	GetPluginMetadataUtf8Fn get_plugin_metadata_utf8 = nullptr;
	InvokeContributionUtf8Fn invoke_contribution_utf8 = nullptr;
	DispatchPluginEventUtf8Fn dispatch_plugin_event_utf8 = nullptr;
	UnloadPluginFn unload_plugin = nullptr;
	GetLastErrorUtf8Fn get_last_error_utf8 = nullptr;
	ShutdownAdapterFn shutdown = nullptr;
};

using InitializeAdapterFn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	NativeHostApi const* native_api,
	AdapterApi* adapter_api);

inline constexpr char kNativePluginEntryPoint[] = "aegisub_plugin_init_v1";
using InitializeNativePluginFn = int32_t(AEGISUB_PLUGIN_BRIDGE_CALL *)(
	NativeHostApi const* native_api,
	uint64_t plugin_handle,
	AdapterApi* adapter_api);

static_assert(std::is_standard_layout_v<NativeHostApi>);
static_assert(std::is_standard_layout_v<AdapterApi>);

} // namespace agi::coreclr
