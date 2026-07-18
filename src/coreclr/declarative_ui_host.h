#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace agi { struct Context; }

namespace agi::coreclr::ui {

using PluginEventDispatcher = std::function<void(
	uint64_t plugin_handle,
	std::string const& event_id,
	std::string const& payload_json)>;

/// Executes a declarative UI host service. UI work is marshalled to the main
/// thread. Unknown service IDs return nullopt so the caller can continue its
/// host-service dispatch chain.
std::optional<std::string> InvokeDeclarativeUiHostService(
	uint64_t plugin_handle,
	Context* invocation_context,
	std::string const& service_id,
	std::string const& request_json,
	PluginEventDispatcher dispatch_event);

/// Closes all modeless views owned by a plugin without dispatching events to
/// the plugin being unloaded.
void CloseDeclarativeUiViewsForPlugin(uint64_t plugin_handle);

} // namespace agi::coreclr::ui
