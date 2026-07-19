#pragma once

#include "automation_scenario.h"
#include "automation_session_service.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/fs_fwd.h>

#include <functional>
#include <string>

namespace aegisub::automation_scenario_runner {

using AutomationStepExecutor = std::function<
	automation_session_service::AutomationSessionResult(
		automation_session_service::AutomationSessionRequest)>;
using CommandStepExecutor = std::function<json::Object(std::string const&)>;
using StepObserver = std::function<void(std::size_t, bool)>;

struct Result {
	int exit_code = 0;
	bool passed = false;
	double duration_ms = 0.0;
	json::Array steps;
};

json::Object SerializeAutomationResult(
	automation_session_service::AutomationSessionResult const& result);

Result Run(
	automation_scenario::Scenario const& scenario,
	std::string const& host,
	agi::fs::path const& artifacts,
	AutomationStepExecutor execute,
	CommandStepExecutor execute_command = {},
	StepObserver observe_step = {});

json::Object SerializeResult(
	std::string const& scenario_name,
	std::string const& host,
	Result result,
	agi::fs::path const& profile,
	agi::fs::path const& artifacts);

}
