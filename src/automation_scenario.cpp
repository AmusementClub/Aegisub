#include "automation_scenario.h"

#include <libaegisub/cajun/reader.h>
#include <libaegisub/io.h>
#include <libaegisub/path.h>

#include <filesystem>
#include <algorithm>
#include <stdexcept>

namespace aegisub::automation_scenario {
namespace {

json::Object TakeRootObject(json::UnknownElement& root) {
	try {
		return std::move(static_cast<json::Object&>(root));
	}
	catch (...) {
		throw std::runtime_error("automation scenario root must be an object");
	}
}

json::UnknownElement const& RequireField(
	json::Object const& object,
	std::string const& key,
	char const* source) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(std::string(source) + " requires '" + key + "'");
	return it->second;
}

std::string RequireString(
	json::Object const& object,
	std::string const& key,
	char const* source) {
	try {
		auto value = static_cast<json::String const&>(RequireField(object, key, source));
		if (value.empty())
			throw std::runtime_error(std::string(source) + " field '" + key + "' cannot be empty");
		return value;
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) {
		throw std::runtime_error(std::string(source) + " field '" + key + "' must be a string");
	}
}

int OptionalPositiveInteger(
	json::Object const& object,
	std::string const& key,
	int fallback,
	char const* source) {
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	try {
		auto value = static_cast<int64_t>(static_cast<json::Integer const&>(it->second));
		if (value <= 0 || value > 24LL * 60 * 60 * 1000)
			throw std::runtime_error(std::string(source) + " field '" + key + "' is out of range");
		return static_cast<int>(value);
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) {
		throw std::runtime_error(std::string(source) + " field '" + key + "' must be an integer");
	}
}

std::vector<std::string> ParseHosts(json::Object const& root) {
	auto it = root.find("hosts");
	if (it == root.end())
		return {"headless", "gui-test"};

	try {
		std::vector<std::string> hosts;
		for (auto const& item : static_cast<json::Array const&>(it->second)) {
			auto host = static_cast<json::String const&>(item);
			if (host != "headless" && host != "gui-test")
				throw std::runtime_error("automation scenario contains an unsupported host: " + host);
			hosts.push_back(std::move(host));
		}
		if (hosts.empty())
			throw std::runtime_error("automation scenario hosts cannot be empty");
		return hosts;
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) {
		throw std::runtime_error("automation scenario field 'hosts' must be an array of strings");
	}
}

std::map<std::string, agi::fs::path> ParseResources(
	json::Object const& root,
	agi::fs::path const& source_path,
	InputOverrides const& overrides) {
	std::map<std::string, std::pair<std::string, bool>> values;
	auto it = root.find("resources");
	if (it != root.end()) {
		try {
			for (auto const& [name, item] : static_cast<json::Object const&>(it->second)) {
				values.emplace(name, std::pair{static_cast<json::String const&>(item), false});
			}
		}
		catch (...) {
			throw std::runtime_error("automation scenario field 'resources' must be an object of strings");
		}
	}

	for (auto const& [name, value] : overrides) {
		auto resource = values.find(name);
		if (resource == values.end())
			throw std::runtime_error("unknown automation scenario input: " + name);
		resource->second = {value, true};
	}

	std::map<std::string, agi::fs::path> resources;
	for (auto const& [name, configured] : values) {
		auto const& [value, overridden] = configured;
		if (value.empty()) {
			resources.emplace(name, agi::fs::path());
			continue;
		}
		auto path = agi::fs::PathFromString(value);
		if (path.is_relative()) {
			path = overridden
				? std::filesystem::absolute(path)
				: source_path.parent_path() / path;
		}
		resources.emplace(name, path.lexically_normal());
	}
	return resources;
}

std::deque<json::Object> ParseSteps(json::Object& root) {
	try {
		std::deque<json::Object> steps;
		auto steps_it = root.find("steps");
		if (steps_it == root.end())
			throw std::runtime_error("automation scenario requires 'steps'");
		auto& array = static_cast<json::Array&>(steps_it->second);
		for (size_t index = 0; index < array.size(); ++index) {
			json::Object step;
			try { step = std::move(static_cast<json::Object&>(array[index])); }
			catch (...) {
				throw std::runtime_error("automation scenario step " + std::to_string(index) + " must be an object");
			}
			RequireString(step, "action", "automation scenario step");
			steps.push_back(std::move(step));
		}
		if (steps.empty())
			throw std::runtime_error("automation scenario steps cannot be empty");
		return steps;
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) {
		throw std::runtime_error("automation scenario field 'steps' must be an array");
	}
}

}

LoadResult Load(
	agi::fs::path const& path,
	InputOverrides const& input_overrides) {
	LoadResult result;
	try {
		auto stream = agi::io::Open(path);
		json::UnknownElement parsed;
		json::Reader::Read(parsed, *stream);
		auto root = TakeRootObject(parsed);

		Scenario scenario;
		scenario.source_path = std::filesystem::absolute(path).lexically_normal();
		try {
			scenario.version = static_cast<int>(static_cast<json::Integer const&>(
				RequireField(root, "version", "automation scenario")));
		}
		catch (...) {
			throw std::runtime_error("automation scenario field 'version' must be an integer");
		}
		if (scenario.version != 1)
			throw std::runtime_error("unsupported automation scenario version: " + std::to_string(scenario.version));

		scenario.name = RequireString(root, "name", "automation scenario");
		scenario.hosts = ParseHosts(root);
		scenario.resources = ParseResources(root, scenario.source_path, input_overrides);
		scenario.default_timeout_ms = OptionalPositiveInteger(
			root, "default_timeout_ms", 120000, "automation scenario");
		scenario.steps = ParseSteps(root);
		result.scenario.emplace(std::move(scenario));
	}
	catch (std::exception const& e) {
		result.error = e.what();
	}
	catch (...) {
		result.error = "could not load automation scenario";
	}
	return result;
}

bool SupportsHost(Scenario const& scenario, std::string const& host) {
	return std::find(scenario.hosts.begin(), scenario.hosts.end(), host) != scenario.hosts.end();
}

}
