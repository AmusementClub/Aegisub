#include "automation_scenario_runner.h"

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace aegisub::automation_scenario_runner {
namespace {

class ScenarioSchemaError final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class ScenarioTimeoutError final : public std::runtime_error {
public:
	ScenarioTimeoutError(
		std::string message,
		bool total,
		std::optional<std::size_t> step)
	: std::runtime_error(std::move(message))
	, total(total)
	, step(std::move(step)) {
	}

	bool total;
	std::optional<std::size_t> step;
};

std::chrono::milliseconds PositiveTimeout(int value) {
	return std::chrono::milliseconds(std::max(value, 1));
}

std::chrono::milliseconds TotalTimeout(
	std::chrono::milliseconds step_timeout,
	std::size_t step_count) {
	if (step_count <= 1)
		return step_timeout;
	auto const max_count = std::chrono::milliseconds::max().count();
	if (step_timeout.count() > max_count / static_cast<int64_t>(step_count))
		return std::chrono::milliseconds::max();
	return step_timeout * static_cast<int64_t>(step_count);
}

void CheckTimeout(
	std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::time_point scenario_deadline,
	std::chrono::steady_clock::time_point step_deadline,
	std::size_t step_index) {
	if (now >= scenario_deadline)
		throw ScenarioTimeoutError(
			"automation scenario exceeded its total timeout",
			true,
			step_index);
	if (now >= step_deadline)
		throw ScenarioTimeoutError(
			"automation scenario step " + std::to_string(step_index) + " exceeded its timeout",
			false,
			step_index);
}

json::UnknownElement const& RequireField(
	json::Object const& object,
	std::string const& key,
	std::string const& source) {
	auto it = object.find(key);
	if (it == object.end())
		throw ScenarioSchemaError(source + " requires '" + key + "'");
	return it->second;
}

std::string RequiredString(
	json::Object const& object,
	std::string const& key,
	std::string const& source) {
	try {
		auto const& value = static_cast<json::String const&>(RequireField(object, key, source));
		if (value.empty())
			throw ScenarioSchemaError(source + " field '" + key + "' cannot be empty");
		return value;
	}
	catch (ScenarioSchemaError const&) {
		throw;
	}
	catch (...) {
		throw ScenarioSchemaError(source + " field '" + key + "' must be a string");
	}
}

std::optional<std::string> OptionalString(
	json::Object const& object,
	std::string const& key,
	std::string const& source) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (...) {
		throw ScenarioSchemaError(source + " field '" + key + "' must be a string");
	}
}

int OptionalInteger(
	json::Object const& object,
	std::string const& key,
	int fallback,
	std::string const& source) {
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	try {
		return static_cast<int>(static_cast<json::Integer const&>(it->second));
	}
	catch (...) {
		throw ScenarioSchemaError(source + " field '" + key + "' must be an integer");
	}
}

bool JsonValueEquals(json::UnknownElement const& left, json::UnknownElement const& right) {
	std::ostringstream left_json;
	std::ostringstream right_json;
	agi::JsonWriter::Write(left, left_json);
	agi::JsonWriter::Write(right, right_json);
	return left_json.str() == right_json.str();
}

json::UnknownElement CloneJsonValue(json::UnknownElement const& value) {
	std::stringstream serialized;
	agi::JsonWriter::Write(value, serialized);
	serialized.seekg(0);
	json::UnknownElement clone;
	json::Reader::Read(clone, serialized);
	return clone;
}

std::string StepTarget(json::Object const& step, std::string const& source) {
	auto target = RequiredString(step, "target", source);
	if (target != "command")
		throw ScenarioSchemaError(source + " has unsupported query target '" + target + "'");
	return target;
}

std::string CaptureName(json::Object const& step, std::string const& source) {
	auto name = RequiredString(step, "name", source);
	if (name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..")
		throw ScenarioSchemaError(source + " capture name must be a plain file name");
	return name;
}

agi::fs::path Resource(
	automation_scenario::Scenario const& scenario,
	json::Object const& step,
	std::string const& field,
	bool required,
	std::string const& source) {
	auto name = OptionalString(step, field, source);
	if (!name) {
		if (required)
			throw ScenarioSchemaError(source + " requires resource field '" + field + "'");
		return {};
	}
	auto it = scenario.resources.find(*name);
	if (it == scenario.resources.end())
		throw ScenarioSchemaError(source + " references unknown resource '" + *name + "'");
	if (required && it->second.empty())
		throw ScenarioSchemaError(source + " resource '" + *name + "' has no path");
	return it->second;
}

std::vector<int> Selection(json::Object const& step, std::string const& source) {
	auto it = step.find("selection");
	if (it == step.end())
		return {};
	try {
		std::vector<int> result;
		for (auto const& value : static_cast<json::Array const&>(it->second))
			result.push_back(static_cast<int>(static_cast<json::Integer const&>(value)));
		return result;
	}
	catch (...) {
		throw ScenarioSchemaError(source + " field 'selection' must be an array of integers");
	}
}

automation_session_service::AutomationSessionRequest BuildRequest(
	automation_scenario::Scenario const& scenario,
	json::Object const& step,
	agi::fs::path const& artifacts,
	std::string const& source) {
	automation_session_service::AutomationSessionRequest request;
	request.script_path = Resource(scenario, step, "script", true, source);
	request.subtitle_path = Resource(scenario, step, "subtitle", false, source);
	request.output_subtitle_path = Resource(scenario, step, "output_subtitle", false, source);
	request.video_path = Resource(scenario, step, "video", false, source);
	request.audio_path = Resource(scenario, step, "audio", false, source);
	request.timecodes_path = Resource(scenario, step, "timecodes", false, source);
	request.keyframes_path = Resource(scenario, step, "keyframes", false, source);
	request.selected_rows = Selection(step, source);
	request.active_row = OptionalInteger(step, "active_row", 0, source);
	request.trace_dir = artifacts;
	request.emit_console_report = false;

	auto macro = OptionalString(step, "macro", source);
	auto filter = OptionalString(step, "filter", source);
	if (macro.has_value() == filter.has_value())
		throw ScenarioSchemaError(source + " requires exactly one of 'macro' or 'filter'");
	request.feature_name = macro ? *macro : *filter;
	request.feature_kind = macro
		? automation_session_service::AutomationSessionFeatureKind::Macro
		: automation_session_service::AutomationSessionFeatureKind::ExportFilter;
	return request;
}

void ValidateScenario(
	automation_scenario::Scenario const& scenario,
	std::string const& host,
	agi::fs::path const& artifacts,
	AutomationStepExecutor const& execute,
	CommandStepExecutor const& execute_command,
	QueryStepExecutor const& execute_query,
	WaitStepExecutor const& execute_wait,
	CaptureStepExecutor const& execute_capture) {
	if (!automation_scenario::SupportsHost(scenario, host))
		throw ScenarioSchemaError("scenario does not support the " + host + " host");

	std::size_t automation_step_count = 0;
	for (size_t index = 0; index < scenario.steps.size(); ++index) {
		auto const& step = scenario.steps[index];
		auto source = "automation scenario step " + std::to_string(index);
		auto action = RequiredString(step, "action", source);
		if (action == "run_automation") {
			if (++automation_step_count > 1)
				throw ScenarioSchemaError("scenario v1 supports at most one run_automation step");
			if (!execute)
				throw ScenarioSchemaError(source + " has no automation executor for the " + host + " host");
			(void)BuildRequest(scenario, step, artifacts, source);
		}
		else if (action == "command") {
			if (!execute_command)
				throw ScenarioSchemaError(source + " command action is not supported by the " + host + " host");
			(void)RequiredString(step, "id", source);
		}
		else if (action == "query") {
			if (!execute_query)
				throw ScenarioSchemaError(source + " query action is not supported by the " + host + " host");
			(void)StepTarget(step, source);
			(void)RequiredString(step, "id", source);
		}
		else if (action == "assert") {
			if (!execute_query)
				throw ScenarioSchemaError(source + " assert action is not supported by the " + host + " host");
			(void)StepTarget(step, source);
			(void)RequiredString(step, "id", source);
			(void)RequiredString(step, "field", source);
			(void)RequireField(step, "equals", source);
		}
		else if (action == "wait") {
			if (!execute_wait)
				throw ScenarioSchemaError(source + " wait action is not supported by the " + host + " host");
			auto milliseconds = OptionalInteger(step, "milliseconds", 0, source);
			if (milliseconds <= 0 || milliseconds > 120000)
				throw ScenarioSchemaError(source + " wait milliseconds must be between 1 and 120000");
		}
		else if (action == "capture") {
			if (!execute_capture)
				throw ScenarioSchemaError(source + " capture action is not supported by the " + host + " host");
			(void)CaptureName(step, source);
		}
		else {
			throw ScenarioSchemaError(source + " has unsupported action '" + action + "'");
		}
	}
}

}

json::Object SerializeAutomationResult(
	automation_session_service::AutomationSessionResult const& result) {
	json::Object output;
	output["exit_code"] = static_cast<int64_t>(result.exit_code);
	output["passed"] = result.passed;
	output["script_loaded"] = result.script_loaded;
	output["feature_found"] = result.feature_found;
	output["validate_ran"] = result.validate_ran;
	output["validate_passed"] = result.validate_passed;
	output["output_saved"] = result.output_saved;
	output["message"] = result.message;
	output["engine_name"] = result.engine_name;
	output["script_name"] = result.script_name;
	output["feature_name"] = result.feature_name;
	output["feature_kind"] = result.feature_kind;
	output["output_subtitle_path"] = agi::fs::PathToGenericString(result.output_subtitle_path);
	return output;
}

Result Run(
	automation_scenario::Scenario const& scenario,
	std::string const& host,
	agi::fs::path const& artifacts,
	AutomationStepExecutor execute,
	CommandStepExecutor execute_command,
	StepObserver observe_step,
	QueryStepExecutor execute_query,
	WaitStepExecutor execute_wait,
	CaptureStepExecutor execute_capture) {
	Result result;
	auto const scenario_started = std::chrono::steady_clock::now();
	auto const step_timeout = PositiveTimeout(scenario.default_timeout_ms);
	auto const scenario_deadline = scenario_started + TotalTimeout(step_timeout, scenario.steps.size());
	try {
		ValidateScenario(
			scenario,
			host,
			artifacts,
			execute,
			execute_command,
			execute_query,
			execute_wait,
			execute_capture);

		for (size_t index = 0; index < scenario.steps.size(); ++index) {
			auto const step_deadline = std::chrono::steady_clock::now() + step_timeout;
			CheckTimeout(
				std::chrono::steady_clock::now(),
				scenario_deadline,
				step_deadline,
				index);
			if (observe_step)
				observe_step(index, true);
			auto const& step = scenario.steps[index];
			auto source = "automation scenario step " + std::to_string(index);
			auto action = RequiredString(step, "action", source);
			auto const step_started = std::chrono::steady_clock::now();
			if (action == "run_automation") {
				auto service_result = execute(BuildRequest(scenario, step, artifacts, source));
				result.exit_code = service_result.exit_code;
				result.passed = service_result.passed;
				auto step_result = SerializeAutomationResult(service_result);
				step_result["index"] = static_cast<int64_t>(index);
				step_result["action"] = action;
				step_result["status"] = result.passed ? "passed" : "failed";
				step_result["duration_ms"] = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - step_started).count();
				result.steps.emplace_back(std::move(step_result));
			}
			else if (action == "command") {
				auto command_id = RequiredString(step, "id", source);
				auto command_result = execute_command(command_id);
				bool invoked = false;
				try { invoked = static_cast<json::Boolean const&>(command_result.at("invoked")); }
				catch (...) { }
				result.exit_code = invoked ? 0 : 1;
				result.passed = invoked;
				command_result["index"] = static_cast<int64_t>(index);
				command_result["action"] = action;
				command_result["status"] = result.passed ? "passed" : "failed";
				command_result["duration_ms"] = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - step_started).count();
				result.steps.emplace_back(std::move(command_result));
			}
			else if (action == "query") {
				auto target = StepTarget(step, source);
				auto query_result = execute_query(target, step);
				result.exit_code = 0;
				result.passed = true;
				query_result["index"] = static_cast<int64_t>(index);
				query_result["action"] = action;
				query_result["status"] = "passed";
				query_result["duration_ms"] = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - step_started).count();
				result.steps.emplace_back(std::move(query_result));
			}
			else if (action == "assert") {
				auto target = StepTarget(step, source);
				auto field = RequiredString(step, "field", source);
				auto query_result = execute_query(target, step);
				auto expected = step.find("equals");
				auto actual = query_result.find(field);
				bool matched = actual != query_result.end() &&
					JsonValueEquals(actual->second, expected->second);
				json::Object assertion;
				assertion["target"] = target;
				assertion["field"] = field;
				assertion["matched"] = matched;
				assertion["expected"] = CloneJsonValue(expected->second);
				if (actual != query_result.end())
					assertion["actual"] = CloneJsonValue(actual->second);
				else
					assertion["error"] = "query did not return the asserted field";
				result.exit_code = matched ? 0 : 1;
				result.passed = matched;
				assertion["index"] = static_cast<int64_t>(index);
				assertion["action"] = action;
				assertion["status"] = matched ? "passed" : "failed";
				assertion["duration_ms"] = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - step_started).count();
				result.steps.emplace_back(std::move(assertion));
			}
			else if (action == "wait") {
				auto milliseconds = OptionalInteger(step, "milliseconds", 0, source);
				execute_wait(std::chrono::milliseconds(milliseconds));
				json::Object wait_result;
				wait_result["milliseconds"] = static_cast<int64_t>(milliseconds);
				wait_result["index"] = static_cast<int64_t>(index);
				wait_result["action"] = action;
				wait_result["status"] = "passed";
				wait_result["duration_ms"] = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - step_started).count();
				result.exit_code = 0;
				result.passed = true;
				result.steps.emplace_back(std::move(wait_result));
			}
			else if (action == "capture") {
				auto name = CaptureName(step, source);
				auto capture_result = execute_capture(
					name,
					artifacts / agi::fs::PathFromString(name + ".png"));
				bool captured = false;
				if (auto it = capture_result.find("captured"); it != capture_result.end()) {
					try { captured = static_cast<json::Boolean const&>(it->second); }
					catch (...) { captured = false; }
				}
				capture_result["name"] = name;
				capture_result["index"] = static_cast<int64_t>(index);
				capture_result["action"] = action;
				capture_result["status"] = captured ? "passed" : "failed";
				capture_result["duration_ms"] = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - step_started).count();
				result.exit_code = captured ? 0 : 1;
				result.passed = captured;
				result.steps.emplace_back(std::move(capture_result));
			}
			else {
				throw ScenarioSchemaError(source + " has unsupported action '" + action + "'");
			}
			CheckTimeout(
				std::chrono::steady_clock::now(),
				scenario_deadline,
				step_deadline,
				index);
			if (observe_step)
				observe_step(index, false);
			if (!result.passed)
				break;
		}
	}
	catch (ScenarioTimeoutError const& e) {
		result.exit_code = 1;
		result.passed = false;
		result.timed_out = true;
		result.timed_out_step = e.step;
		json::Object error;
		error["error"] = e.what();
		error["error_kind"] = "timeout";
		error["phase"] = e.total ? "scenario" : "step";
		error["timed_out"] = true;
		if (e.step)
			error["step_index"] = static_cast<int64_t>(*e.step);
		result.steps.emplace_back(std::move(error));
	}
	catch (ScenarioSchemaError const& e) {
		result.exit_code = 64;
		result.passed = false;
		json::Object error;
		error["error"] = e.what();
		error["error_kind"] = "schema";
		error["phase"] = "scenario";
		result.steps.emplace_back(std::move(error));
	}
	catch (std::exception const& e) {
		result.exit_code = 2;
		result.passed = false;
		json::Object error;
		error["error"] = e.what();
		error["error_kind"] = "runtime";
		error["phase"] = "scenario";
		result.steps.emplace_back(std::move(error));
	}
	catch (...) {
		result.exit_code = 2;
		result.passed = false;
		json::Object error;
		error["error"] = "unknown automation scenario failure";
		error["error_kind"] = "runtime";
		error["phase"] = "scenario";
		result.steps.emplace_back(std::move(error));
	}
	result.duration_ms = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - scenario_started).count();
	return result;
}

json::Object SerializeResult(
	std::string const& scenario_name,
	std::string const& host,
	Result result,
	agi::fs::path const& profile,
	agi::fs::path const& artifacts) {
	json::Object output;
	output["version"] = static_cast<int64_t>(1);
	output["scenario"] = scenario_name;
	output["host"] = host;
	output["exit_code"] = static_cast<int64_t>(result.exit_code);
	output["passed"] = result.passed;
	output["duration_ms"] = result.duration_ms;
	output["timed_out"] = result.timed_out;
	if (result.timed_out_step)
		output["timed_out_step"] = static_cast<int64_t>(*result.timed_out_step);
	output["profile"] = agi::fs::PathToGenericString(profile);
	output["artifacts"] = agi::fs::PathToGenericString(artifacts);
	output["steps"] = std::move(result.steps);
	return output;
}

}
