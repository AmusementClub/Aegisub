#ifdef WITH_AVISYNTH

#include "../../src/avisynth_legacy_path.h"
#include "../../src/avisynth_wrap.h"
#include "../../src/options.h"

#include <avisynth.h>

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
#ifdef _WIN32
std::string CurrentCodePage() {
	return std::to_string(GetACP());
}
#else
std::string CurrentCodePage() {
	return "non-windows";
}
#endif

std::mutex main_queue_mutex;
std::deque<agi::dispatch::Thunk> main_queue;
std::thread::id main_thread_id;

std::size_t FlushMainQueue() {
	std::size_t executed = 0;
	while (true) {
		agi::dispatch::Thunk thunk;
		{
			std::lock_guard<std::mutex> lock(main_queue_mutex);
			if (main_queue.empty())
				return executed;

			thunk = std::move(main_queue.front());
			main_queue.pop_front();
		}

		++executed;
		thunk();
	}
}

class ScopedDispatch {
public:
	ScopedDispatch() {
		main_thread_id = std::this_thread::get_id();
		agi::dispatch::Init([](agi::dispatch::Thunk thunk) {
			std::lock_guard<std::mutex> lock(main_queue_mutex);
			main_queue.emplace_back(std::move(thunk));
		}, [] {
			return std::this_thread::get_id() == main_thread_id;
		}, [] {
			return FlushMainQueue();
		});
	}
};

class ScopedLogging {
public:
	ScopedLogging(agi::fs::path const& directory) {
		agi::log::log = new agi::log::LogSink;
		agi::log::log->Subscribe(std::make_unique<agi::log::JsonEmitter>(directory));
	}

	~ScopedLogging() {
		delete agi::log::log;
		agi::log::log = nullptr;
	}
};

class ScopedConfigContext {
	agi::Options options;
	agi::Path path_tokens;
	agi::fs::path user_root;
	agi::fs::path data_root;

public:
	ScopedConfigContext(agi::fs::path const& scenario_root)
	: options("", R"json(
{
  "Provider": {
    "Avisynth": {
      "Runtime Path": "",
      "Allow Ancient": false,
      "Memory Max": 0
    }
  }
}
)json", agi::Options::FLUSH_SKIP)
	, user_root(scenario_root / "user")
	, data_root(scenario_root / "data") {
		path_tokens.SetToken("?local", (scenario_root / "cache").string());
		path_tokens.SetToken("?temp", (scenario_root / "temp").string());
		path_tokens.SetToken("?user", user_root.string());
		path_tokens.SetToken("?data", data_root.string());
		config::opt = &options;
		config::path = &path_tokens;
	}

	~ScopedConfigContext() {
		config::opt = nullptr;
		config::path = nullptr;
	}

	agi::fs::path const& UserRoot() const { return user_root; }
	agi::fs::path const& DataRoot() const { return data_root; }
	agi::fs::path UserAutoloadDir() const { return user_root / "runtimes" / "avs-plugins"; }
};

struct ScenarioResult {
	std::string label;
	agi::fs::path user_root;
	agi::fs::path autoload_dir;
	std::optional<std::string> legacy_path;
	std::string loaded_library;
	std::string autoload_dirs;
	std::string error;
	bool script_env_present = false;
	bool neo_env_present = false;
	bool add_autoload_dir_exists = false;
	std::vector<agi::log::SinkMessage> plugin_logs;
};

std::vector<agi::log::SinkMessage> CollectPluginLogs(std::size_t start_index) {
	auto messages = agi::log::log->GetMessages();
	std::vector<agi::log::SinkMessage> filtered;
	for (std::size_t i = start_index; i < messages.size(); ++i) {
		if (messages[i].section && std::string_view(messages[i].section) == "provider/avisynth/plugins")
			filtered.push_back(messages[i]);
	}
	return filtered;
}

ScenarioResult RunScenario(std::string label, agi::fs::path const& scenario_root) {
	std::error_code ec;
	std::filesystem::remove_all(scenario_root, ec);
	std::filesystem::create_directories(scenario_root / "user" / "runtimes" / "avs-plugins");
	std::filesystem::create_directories(scenario_root / "data");

	ScopedConfigContext config(scenario_root);
	ScenarioResult result;
	result.label = std::move(label);
	result.user_root = config.UserRoot();
	result.autoload_dir = config.UserAutoloadDir();
	result.legacy_path = avisynth::TryGetLegacyPathString(result.autoload_dir);
	auto log_start = agi::log::log->GetMessages().size();

	try {
		AviSynthWrapper wrapper;
		result.loaded_library = avisynth::GetLoadedLibrary();
		result.script_env_present = wrapper.GetEnv() != nullptr;
		if (auto *env = wrapper.GetEnv()) {
			result.add_autoload_dir_exists = env->FunctionExists("AddAutoloadDir");
			PNeoEnv neo_env(env);
			result.neo_env_present = !!neo_env;
			if (!!neo_env) {
				if (char *dirs = neo_env->ListAutoloadDirs(); dirs && *dirs)
					result.autoload_dirs = dirs;
			}
		}
	}
	catch (AvisynthError const& err) {
		result.error = err.msg ? err.msg : "AvisynthError without message";
	}
	catch (std::exception const& err) {
		result.error = err.what();
	}

	result.plugin_logs = CollectPluginLogs(log_start);
	return result;
}

char SeverityCode(agi::log::Severity severity) {
	return agi::log::Severity_ID[static_cast<int>(severity)];
}

void PrintScenario(ScenarioResult const& result) {
	std::cout
		<< "scenario=" << result.label << "\n"
		<< "user_root=" << agi::fs::PathToString(result.user_root) << "\n"
		<< "autoload_dir=" << agi::fs::PathToString(result.autoload_dir) << "\n"
		<< "legacy_path=" << (result.legacy_path ? *result.legacy_path : std::string("<none>")) << "\n"
		<< "loaded_library=" << (result.loaded_library.empty() ? std::string("<none>") : result.loaded_library) << "\n"
		<< "script_env_present=" << (result.script_env_present ? 1 : 0) << "\n"
		<< "neo_env_present=" << (result.neo_env_present ? 1 : 0) << "\n"
		<< "function_AddAutoloadDir=" << (result.add_autoload_dir_exists ? 1 : 0) << "\n"
		<< "autoload_dirs=" << (result.autoload_dirs.empty() ? std::string("<empty>") : result.autoload_dirs) << "\n"
		<< "error=" << (result.error.empty() ? std::string("<none>") : result.error) << "\n"
		<< "plugin_log_count=" << result.plugin_logs.size() << "\n";

	for (auto const& message : result.plugin_logs) {
		std::cout << "plugin_log[" << SeverityCode(message.severity) << "]=" << message.message << "\n";
	}
	std::cout << std::flush;
}

bool BaselineSucceeded(ScenarioResult const& result) {
	return result.error.empty()
		&& result.script_env_present
		&& result.neo_env_present
		&& !result.autoload_dirs.empty();
}
}

int main(int argc, char **argv) {
	ScopedDispatch dispatch;
	auto const executable = argc > 0 ? std::filesystem::absolute(argv[0]) : std::filesystem::current_path();
	auto const executable_dir = executable.parent_path();
	ScopedLogging logging(executable_dir / "avisynth-smoke-logs");

	std::cout << "current_acp=" << CurrentCodePage() << "\n";
	std::cout << "session_log_file=" << agi::fs::PathToString(agi::log::GetSessionLogFile()) << "\n";

	auto const workspace_root = executable_dir / "avisynth-autoload-runtime-smoke-work";
	auto const ascii_root = workspace_root / "ascii-root";
	auto const non_ascii_root = workspace_root / std::filesystem::path(L"non-ascii-\u4e2d\u6587-\U0001f600");

	auto const ascii = RunScenario("ascii", ascii_root);
	auto const non_ascii = RunScenario("non_ascii", non_ascii_root);

	PrintScenario(ascii);
	PrintScenario(non_ascii);

	if (!BaselineSucceeded(ascii)) {
		std::cerr << "ASCII baseline autoload registration did not succeed." << std::endl;
		return 2;
	}

	if (!non_ascii.error.empty()) {
		std::cerr << "Non-ASCII scenario failed to create an Avisynth environment." << std::endl;
		return 3;
	}

	return 0;
}

#else

int main() {
	return 0;
}

#endif
