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

std::string InvokeDependencyControlService(
	std::string const& operation_id,
	std::string const& request_json);
void OpenDependencyControlPackageManager(agi::Context* context);
void ShutdownManagedPluginRuntime() noexcept;

} // namespace Automation4
