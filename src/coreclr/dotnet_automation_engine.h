#pragma once

#include "automation/engine/automation_engine.h"

#include <string>

namespace agi { struct Context; }

namespace Automation4 {

class DotNetAutomationEngine final : public AutomationEngine {
public:
	std::string EngineName() const override;
	std::string FilenamePattern() const override;
	bool SupportsFile(agi::fs::path const& filename) const override;
	std::unique_ptr<AutomationScriptInstance> LoadScript(agi::fs::path const& filename) const override;
};

/// Load a discovered Plugin Bridge payload by absolute manifest path.
/// Accepts managed-store names (*.aegisub-plugin.json) and the shorter installed
/// app-local name (plugin.json). Autoload file globs still use SupportsFile and
/// only match *.aegisub-plugin.json so bare plugin.json is not swept up from
/// traditional Automation directories.
std::unique_ptr<AutomationScriptInstance> LoadPluginBridgeManifest(
	agi::fs::path const& filename);

/// Invoke a loaded plugin's serviceProvider contribution by id.
/// Plugins register these when their script instance commits features; the host
/// does not hardcode which package provides which contribution.
std::string InvokePluginServiceContribution(
	std::string const& contribution_id,
	std::string const& operation_id,
	std::string const& request_json);

void ShutdownManagedPluginRuntime() noexcept;

} // namespace Automation4
