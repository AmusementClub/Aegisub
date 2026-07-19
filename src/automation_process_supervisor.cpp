#include "automation_process_supervisor.h"
#include "automation_process_supervisor_test.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/path.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
// windows.h maps CreateDirectory to CreateDirectoryW and breaks agi::fs::CreateDirectory.
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aegisub::automation_process_supervisor {
namespace {

using Clock = std::chrono::steady_clock;

// Production floors. Tests inject their own via testing::Hooks.
constexpr auto kDefaultStartupTimeout = std::chrono::seconds(30);
constexpr auto kDefaultTeardownTimeout = std::chrono::seconds(120);
constexpr auto kDefaultExitTimeout = std::chrono::seconds(5);

agi::fs::path Marker(
	agi::fs::path const& control_directory,
	std::string const& name) {
	return control_directory / agi::fs::PathFromString(name);
}

void WriteMarker(agi::fs::path const& path) {
	agi::fs::CreateDirectory(path.parent_path());
	auto stream = agi::io::Save(path);
	stream.Get() << "ready\n";
}

bool Exists(agi::fs::path const& path) {
	std::error_code error;
	return std::filesystem::exists(path, error);
}

void RemoveControlDirectory(agi::fs::path const& path) {
	std::error_code error;
	std::filesystem::remove_all(path, error);
}

#ifdef _WIN32

std::wstring Utf8ToWide(std::string const& value) {
	if (value.empty())
		return {};
	int const size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (size <= 0)
		throw std::runtime_error("could not convert worker argument to UTF-16");
	std::wstring result(static_cast<std::size_t>(size), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), result.data(), size) != size)
		throw std::runtime_error("could not convert worker argument to UTF-16");
	return result;
}

std::wstring QuoteArgument(std::wstring const& value) {
	if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos)
		return value;

	std::wstring quoted(1, L'\"');
	std::size_t slashes = 0;
	for (wchar_t character : value) {
		if (character == L'\\') {
			++slashes;
			continue;
		}
		if (character == L'\"') {
			quoted.append(slashes * 2 + 1, L'\\');
			quoted.push_back(L'\"');
		}
		else {
			quoted.append(slashes, L'\\');
			quoted.push_back(character);
		}
		slashes = 0;
	}
	quoted.append(slashes * 2, L'\\');
	quoted.push_back(L'\"');
	return quoted;
}

class ChildProcess final {
	PROCESS_INFORMATION process{};
	HANDLE output = INVALID_HANDLE_VALUE;
	HANDLE error_output = INVALID_HANDLE_VALUE;
	bool started = false;

public:
	~ChildProcess() {
		if (started) {
			if (process.hThread) CloseHandle(process.hThread);
			if (process.hProcess) CloseHandle(process.hProcess);
		}
		if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
		if (error_output != INVALID_HANDLE_VALUE) CloseHandle(error_output);
	}

	bool Start(
		std::vector<std::string> const& arguments,
		agi::fs::path const& log_directory,
		std::string& error) {
		if (arguments.empty()) {
			error = "automation worker has no executable argument";
			return false;
		}
		try {
			std::wstring executable = Utf8ToWide(arguments.front());
			std::wstring command_line;
			for (auto const& argument : arguments) {
				if (!command_line.empty()) command_line.push_back(L' ');
				command_line += QuoteArgument(Utf8ToWide(argument));
			}
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
			output = CreateFileW(
				(log_directory / L"worker-stdout.log").c_str(),
				GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			error_output = CreateFileW(
				(log_directory / L"worker-stderr.log").c_str(),
				GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (output == INVALID_HANDLE_VALUE || error_output == INVALID_HANDLE_VALUE) {
				error = "could not open automation worker logs";
				return false;
			}
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startup.hStdOutput = output;
			startup.hStdError = error_output;
			process = {};
			if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
				TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
				// PROCESS_INFORMATION contents are undefined after a failed create.
				process = {};
				error = "could not start automation worker: " + std::to_string(GetLastError());
				return false;
			}
			started = true;
			return true;
		}
		catch (std::exception const& exception) {
			process = {};
			started = false;
			error = exception.what();
			return false;
		}
	}

	bool Poll(int& exit_code) const {
		DWORD code = 0;
		if (!GetExitCodeProcess(process.hProcess, &code) || code == STILL_ACTIVE)
			return false;
		exit_code = code <= static_cast<DWORD>(std::numeric_limits<int>::max())
			? static_cast<int>(code)
			: 2;
		return true;
	}

	void Terminate() const {
		TerminateProcess(process.hProcess, 1);
		WaitForSingleObject(process.hProcess, 5000);
	}
};

#else

class ChildProcess final {
	pid_t pid = -1;

public:
	bool Start(
		std::vector<std::string> const& arguments,
		agi::fs::path const& log_directory,
		std::string& error) {
		if (arguments.empty()) {
			error = "automation worker has no executable argument";
			return false;
		}
		pid = fork();
		if (pid < 0) {
			error = "could not fork automation worker";
			return false;
		}
		if (pid == 0) {
			std::freopen((log_directory / "worker-stdout.log").c_str(), "w", stdout);
			std::freopen((log_directory / "worker-stderr.log").c_str(), "w", stderr);
			std::vector<char*> argv;
			argv.reserve(arguments.size() + 1);
			for (auto const& argument : arguments)
				argv.push_back(const_cast<char*>(argument.c_str()));
			argv.push_back(nullptr);
			execvp(argv.front(), argv.data());
			_exit(127);
		}
		return true;
	}

	bool Poll(int& exit_code) const {
		int status = 0;
		auto const result = waitpid(pid, &status, WNOHANG);
		if (result <= 0)
			return false;
		exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 2;
		return true;
	}

	void Terminate() const {
		kill(pid, SIGKILL);
		int status = 0;
		waitpid(pid, &status, 0);
	}
};

#endif

} // namespace

// Internal entry used by production Run() and testing::Run(). Not in the public header.
Result RunWithHooks(
	std::vector<std::string> const& arguments,
	agi::fs::path const& control_directory,
	int step_timeout_ms,
	std::size_t step_count,
	testing::Hooks hooks) {
	Result result;
	result.phase = "start";
	RemoveControlDirectory(control_directory);
	agi::fs::CreateDirectory(control_directory);

	if (!hooks.start || !hooks.poll || !hooks.terminate || !hooks.now || !hooks.sleep) {
		result.error = "automation process supervisor hooks are incomplete";
		RemoveControlDirectory(control_directory);
		return result;
	}

	if (!hooks.start(arguments, control_directory.parent_path(), result.error)) {
		RemoveControlDirectory(control_directory);
		return result;
	}
	result.started = true;
	result.phase = "startup";

	auto const step_timeout = std::chrono::milliseconds(
		step_timeout_ms > 0 ? step_timeout_ms : 1);
	auto deadline = hooks.now() + hooks.timeouts.startup;
	bool runtime_ready = false;
	// True only after scenario work ends (scenario-complete marker or last step-done).
	// Must not be pre-set for empty scenarios during startup, or startup timeouts
	// would be misclassified as teardown.
	bool scenario_complete = false;
	bool shutdown_complete = false;
	std::optional<std::size_t> active_step;
	std::vector<bool> running_seen(step_count, false);
	std::vector<bool> done_seen(step_count, false);

	auto enter_teardown = [&](Clock::time_point now) {
		if (scenario_complete)
			return;
		scenario_complete = true;
		active_step.reset();
		result.phase = "teardown";
		deadline = now + hooks.timeouts.teardown;
	};

	while (true) {
		if (hooks.poll(result.exit_code)) {
			RemoveControlDirectory(control_directory);
			return result;
		}

		auto const now = hooks.now();
		if (!runtime_ready && Exists(Marker(control_directory, "runtime-ready"))) {
			runtime_ready = true;
			if (step_count == 0) {
				enter_teardown(now);
			}
			else {
				result.phase = "step";
				deadline = now + step_timeout;
			}
		}

		for (std::size_t index = 0; index < step_count; ++index) {
			if (!running_seen[index] && Exists(Marker(control_directory,
				"step-" + std::to_string(index) + "-running"))) {
				running_seen[index] = true;
				active_step = index;
				if (!scenario_complete) {
					result.phase = "step";
					deadline = now + step_timeout;
				}
			}
			if (!done_seen[index] && Exists(Marker(control_directory,
				"step-" + std::to_string(index) + "-done"))) {
				done_seen[index] = true;
				if (active_step == index)
					active_step.reset();
				if (index + 1 == step_count) {
					// Last step completed successfully enough to mark done.
					// Prefer worker-scenario-complete for exception paths that
					// never write step-done; keep this as a defense in depth.
					enter_teardown(now);
				}
				else if (!scenario_complete) {
					deadline = now + step_timeout;
				}
			}
		}

		// Scenario work finished (including schema/runtime failures that never
		// wrote step-done). Enter long teardown before managed unload ends.
		if (!scenario_complete && Exists(Marker(control_directory, "worker-scenario-complete"))) {
			enter_teardown(now);
		}

		if (!shutdown_complete && Exists(Marker(control_directory, "worker-shutdown-complete"))) {
			shutdown_complete = true;
			result.phase = "exit";
			// Result is on disk; only wait briefly for the process to exit.
			deadline = now + hooks.timeouts.exit;
		}

		if (now >= deadline) {
			hooks.terminate();
			result.timed_out = true;
			result.exit_code = 1;
			result.step_index = active_step;
			if (shutdown_complete) {
				result.phase = "exit";
				result.error = "automation worker timed out after shutdown complete";
			}
			else if (scenario_complete) {
				result.phase = "teardown";
				result.error = "automation worker timed out during runtime teardown";
			}
			else if (active_step) {
				result.phase = "step";
				result.error = "automation scenario step "
					+ std::to_string(*active_step) + " timed out";
			}
			else if (runtime_ready) {
				result.phase = "step";
				result.error = "automation worker timed out between scenario steps";
			}
			else {
				result.phase = "startup";
				result.error = "automation worker runtime startup timed out";
			}
			RemoveControlDirectory(control_directory);
			return result;
		}

		hooks.sleep(std::chrono::milliseconds(20));
	}
}

Result Run(
	std::vector<std::string> const& arguments,
	agi::fs::path const& control_directory,
	int step_timeout_ms,
	std::size_t step_count) {
	ChildProcess child;
	testing::Hooks hooks;
	hooks.start = [&](std::vector<std::string> const& args, agi::fs::path const& logs, std::string& error) {
		return child.Start(args, logs, error);
	};
	hooks.poll = [&](int& exit_code) {
		return child.Poll(exit_code);
	};
	hooks.terminate = [&] {
		child.Terminate();
	};
	hooks.now = [] {
		return Clock::now();
	};
	hooks.sleep = [](std::chrono::milliseconds duration) {
		std::this_thread::sleep_for(duration);
	};
	hooks.timeouts.startup = kDefaultStartupTimeout;
	hooks.timeouts.teardown = kDefaultTeardownTimeout;
	hooks.timeouts.exit = kDefaultExitTimeout;
	return RunWithHooks(arguments, control_directory, step_timeout_ms, step_count, std::move(hooks));
}

void MarkRuntimeReady(agi::fs::path const& control_directory) {
	WriteMarker(Marker(control_directory, "runtime-ready"));
}

void MarkStep(agi::fs::path const& control_directory, std::size_t index, bool started) {
	WriteMarker(Marker(control_directory,
		"step-" + std::to_string(index) + (started ? "-running" : "-done")));
}

void MarkWorkerScenarioComplete(agi::fs::path const& control_directory) {
	WriteMarker(Marker(control_directory, "worker-scenario-complete"));
}

void MarkWorkerShutdownComplete(agi::fs::path const& control_directory) {
	WriteMarker(Marker(control_directory, "worker-shutdown-complete"));
}

}

namespace aegisub::automation_process_supervisor::testing {

Result Run(
	std::vector<std::string> const& arguments,
	agi::fs::path const& control_directory,
	int step_timeout_ms,
	std::size_t step_count,
	Hooks hooks) {
	return aegisub::automation_process_supervisor::RunWithHooks(
		arguments, control_directory, step_timeout_ms, step_count, std::move(hooks));
}

}
