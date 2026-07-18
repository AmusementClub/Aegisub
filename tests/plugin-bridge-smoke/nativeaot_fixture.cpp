#include "../../src/coreclr/bridge_abi.h"

#ifdef _WIN32
#define AEGISUB_NATIVEAOT_FIXTURE_EXPORT __declspec(dllexport)
#else
#define AEGISUB_NATIVEAOT_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

using namespace agi::coreclr;

#ifdef AEGISUB_NATIVEAOT_FIXTURE_MISSING_EXPORT
extern "C" AEGISUB_NATIVEAOT_FIXTURE_EXPORT int32_t
AEGISUB_PLUGIN_BRIDGE_CALL aegisub_plugin_fixture_wrong_entry(
	NativeHostApi const*,
	uint64_t,
	AdapterApi*) {
	return static_cast<int32_t>(BridgeStatus::Success);
}
#else
extern "C" AEGISUB_NATIVEAOT_FIXTURE_EXPORT int32_t
AEGISUB_PLUGIN_BRIDGE_CALL aegisub_plugin_init_v1(
	NativeHostApi const*,
	uint64_t,
	AdapterApi* adapter_api) {
	if (!adapter_api)
		return static_cast<int32_t>(BridgeStatus::InvalidArgument);
	*adapter_api = {};
	adapter_api->abi_version = kNativeBridgeAbiVersion + 1;
	adapter_api->size = sizeof(AdapterApi);
	return static_cast<int32_t>(BridgeStatus::Success);
}
#endif
