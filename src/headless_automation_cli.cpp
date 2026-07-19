#include "headless_automation_cli.h"

#include <libaegisub/path.h>

namespace aegisub::headless_automation_cli {
namespace {

std::optional<std::string> RequireValue(
	std::vector<std::string> const& args,
	size_t& index,
	std::string const& option,
	std::string& error) {
	if (++index >= args.size()) {
		error = option + " requires a value\n" + Usage();
		return std::nullopt;
	}
	return args[index];
}

}

ParseResult Parse(std::vector<std::string> const& args) {
	ParseResult result;
	if (args.size() < 2 || args[1] != "--headless")
		return result;
	result.requested = true;

	if (args.size() < 3 || args[2] != "run") {
		result.error = "--headless requires the 'run' operation\n" + Usage();
		return result;
	}

	RunRequest request;
	for (size_t i = 3; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--scenario") {
			auto value = RequireValue(args, i, arg, result.error);
			if (!value) return result;
			request.scenario_path = agi::fs::PathFromString(*value);
		}
		else if (arg == "--input") {
			auto value = RequireValue(args, i, arg, result.error);
			if (!value) return result;
			auto separator = value->find('=');
			if (separator == std::string::npos || separator == 0) {
				result.error = "--input requires name=value\n" + Usage();
				return result;
			}
			request.inputs.emplace_back(value->substr(0, separator), value->substr(separator + 1));
		}
		else if (arg == "--profile-dir") {
			auto value = RequireValue(args, i, arg, result.error);
			if (!value) return result;
			request.profile_directory = agi::fs::PathFromString(*value);
		}
		else if (arg == "--artifacts") {
			auto value = RequireValue(args, i, arg, result.error);
			if (!value) return result;
			request.artifacts_directory = agi::fs::PathFromString(*value);
		}
		else if (arg == "--keep-profile") {
			request.keep_profile = true;
		}
		else if (arg == "--automation-worker") {
			request.internal_worker = true;
		}
		else {
			result.error = "unrecognized --headless run argument: " + arg + "\n" + Usage();
			return result;
		}
	}

	if (request.scenario_path.empty()) {
		result.error = "--headless run requires --scenario\n" + Usage();
		return result;
	}
	result.request.emplace(std::move(request));
	return result;
}

std::string Usage() {
	return
		"Usage:\n"
		"  Aegisub.exe --headless run --scenario <file> [--input name=value ...]\n"
		"    [--profile-dir <path>] [--artifacts <path>] [--keep-profile]";
}

}
