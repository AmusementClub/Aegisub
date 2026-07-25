#pragma once

struct lua_State;

namespace Automation4 {

/// Register the generic Plugin Bridge Lua API on the active Lua state.
/// Currently: aegisub.plugin.invoke(contribution_id, operation_id [, request]).
/// DependencyControl and other plugins are loaded as normal include modules
/// (e.g. require("l0.DependencyControl")) and call this invoke API.
void RegisterPluginLuaApi(lua_State *L);

} // namespace Automation4
