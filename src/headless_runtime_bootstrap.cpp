// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "headless_runtime_bootstrap.h"

#include "command/command.h"
#include "include/aegisub/hotkey.h"

#include "aegisublocale.h"
#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "crash_writer.h"
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "format.h"
#include "headless_cli.h"
#include "headless_playback_probe.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "subtitles_provider_libass.h"
#include "utils.h"
#include "version.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/locale.hpp>

#include <condition_variable>
#include <deque>
#include <iostream>
#include <locale>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <wx/image.h>
#include <wx/init.h>
#include <wx/log.h>

namespace {

void ReportHeadlessError(std::string const& title, std::string const& message) {
	if (message.empty())
		return;
	if (!title.empty())
		std::cerr << "[" << title << "] ";
	std::cerr << message << std::endl;
}

void InitializeGlobalLocale() {
	auto locale = boost::locale::generator().generate("");

	using codecvt = std::codecvt<wchar_t, char, std::mbstate_t>;
	int result = std::codecvt_base::error;
	if (std::has_facet<codecvt>(locale)) {
		wchar_t test[] = L"\xFFFE";
		char buff[8];
		auto mb = std::mbstate_t();
		const wchar_t* from_next;
		char* to_next;
		result = std::use_facet<codecvt>(locale).out(
			mb,
			test, std::end(test), from_next,
			buff, std::end(buff), to_next);
	}

	if (result != std::codecvt_base::ok)
		locale = boost::locale::generator().generate("en_US.UTF-8");
	std::locale::global(locale);
}

class HeadlessMainThreadPump {
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<agi::dispatch::Thunk> jobs;
	std::thread::id main_thread_id = std::this_thread::get_id();

public:
	void Post(agi::dispatch::Thunk thunk) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			jobs.emplace_back(std::move(thunk));
		}
		cv.notify_all();
	}

	bool IsMainThread() const {
		return std::this_thread::get_id() == main_thread_id;
	}

	void Notify() {
		cv.notify_all();
	}

	std::size_t Flush() {
		std::size_t ran = 0;
		while (true) {
			agi::dispatch::Thunk thunk;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (jobs.empty())
					break;
				thunk = std::move(jobs.front());
				jobs.pop_front();
			}
			thunk();
			++ran;
		}
		return ran;
	}

	template<typename Predicate>
	void RunUntil(Predicate&& done) {
		while (!done()) {
			if (Flush() != 0)
				continue;

			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [&] {
				return done() || !jobs.empty();
			});
		}

		while (Flush() != 0) {
		}
	}
};

class HeadlessRuntimeEnvironment {
	wxInitializer wx_initializer;
	HeadlessMainThreadPump main_thread_pump;
	AegisubLocale locale;
	bool commands_initialized = false;
	bool runtime_ready = false;

public:
	bool Initialize(std::string& error) {
		if (!wx_initializer.IsOk()) {
			error = "failed to initialize wx runtime for headless mode";
			return false;
		}

		try {
			(void)wxLog::GetActiveTarget();
			InitializeGlobalLocale();

			agi::dispatch::Init(
				[this](agi::dispatch::Thunk thunk) {
					main_thread_pump.Post(std::move(thunk));
				},
				[this] {
					return main_thread_pump.IsMainThread();
				},
				[this] {
					return main_thread_pump.Flush();
				});

			config::path = new agi::Path;
			crash_writer::Initialize(config::path->Decode("?user"));

			agi::log::log = new agi::log::LogSink;
#ifdef _DEBUG
			agi::log::log->Subscribe(agi::make_unique<agi::log::EmitSTDOUT>());
#endif

#ifdef __WXMSW__
			try {
				auto conf_local(config::path->Decode("?data/config.json"));
				std::unique_ptr<std::istream> local_config(agi::io::Open(conf_local));
				config::opt = new agi::Options(conf_local, GET_DEFAULT_CONFIG(default_config));
				config::path->SetToken("?user", config::path->Decode("?data"));
				config::path->SetToken("?local", config::path->Decode("?data"));
				crash_writer::Initialize(config::path->Decode("?user"));
			}
			catch (agi::fs::FileSystemError const&) {
			}
#endif

			perf_trace::Initialize(GetAegisubLongVersionString());
			auto path_log = config::path->Decode("?user/log/");
			agi::fs::CreateDirectory(path_log);
			agi::log::log->Subscribe(agi::make_unique<agi::log::JsonEmitter>(path_log));
			CleanCache(path_log, "*.ndjson", 10, 100, 24 * 60 * 60);
			CleanCache(path_log, "*.json", 10, 100, 24 * 60 * 60);

			if (!config::opt)
				config::opt = new agi::Options(config::path->Decode("?user/config.json"), GET_DEFAULT_CONFIG(default_config));
			boost::interprocess::ibufferstream stream((const char *)default_config_platform, sizeof(default_config_platform));
			config::opt->ConfigNext(stream);
			try {
				config::opt->ConfigUser();
			}
			catch (agi::Exception const& err) {
				ReportHeadlessError("config", agi::format("Configuration file is invalid. Error reported:\n%s", err.GetMessage()));
			}

#ifdef _WIN32
			if (OPT_GET("App/First Start")->GetBool()) {
				try {
					auto installer_config = agi::io::Open(config::path->Decode("?data/installer_config.json"));
					config::opt->ConfigNext(*installer_config.get());
				}
				catch (agi::fs::FileSystemError const&) {
				}
			}
#endif

			cmd::init_builtin_commands();
			commands_initialized = true;
			hotkey::init();

			config::mru = new agi::MRUManager(config::path->Decode("?user/mru.json"), GET_DEFAULT_CONFIG(default_mru), config::opt);

			agi::util::SetThreadName("AegiMain");
			srand(time(nullptr));
			setlocale(LC_NUMERIC, "C");
			setlocale(LC_CTYPE, "C");
			OPT_SET("Version/Last Version")->SetInt(GetSVNRevision());

			auto lang = OPT_GET("App/Language")->GetString();
			if (lang.empty() || !locale.HasLanguage(lang))
				lang = "en_US";
			locale.Init(lang);

			Automation4::ScriptFactory::Register(agi::make_unique<Automation4::LuaScriptFactory>());
			libass::CacheFonts();
			AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
			AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());
			wxImage::AddHandler(new wxPNGHandler);

			runtime_ready = true;
			return true;
		}
		catch (agi::Exception const& err) {
			error = err.GetMessage();
		}
		catch (std::exception const& err) {
			error = err.what();
		}
		catch (...) {
			error = "Unhandled exception during headless runtime initialization";
		}

		Shutdown();
		return false;
	}

	~HeadlessRuntimeEnvironment() {
		Shutdown();
	}

	HeadlessMainThreadPump& MainThreadPump() {
		return main_thread_pump;
	}

private:
	void Shutdown() {
		if (config::opt) {
			delete config::opt;
			config::opt = nullptr;
		}
		if (config::mru) {
			delete config::mru;
			config::mru = nullptr;
		}
		if (commands_initialized) {
			hotkey::clear();
			cmd::clear();
			commands_initialized = false;
		}

		if (config::global_scripts) {
			delete config::global_scripts;
			config::global_scripts = nullptr;
		}

		AssExportFilterChain::Clear();
		perf_trace::Shutdown();

		if (agi::log::log) {
			delete agi::log::log;
			agi::log::log = nullptr;
		}

		crash_writer::Cleanup();

		runtime_ready = false;
	}
};

template<typename Result, typename Start>
Result RunAsyncWithPump(HeadlessMainThreadPump& pump, Start&& start) {
	std::mutex mutex;
	std::optional<Result> result;

	start([&](Result finished) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			result.emplace(std::move(finished));
		}
		pump.Notify();
	});

	pump.RunUntil([&] {
		std::lock_guard<std::mutex> lock(mutex);
		return result.has_value();
	});

	std::lock_guard<std::mutex> lock(mutex);
	return std::move(*result);
}

int RunParsedHeadlessCli(HeadlessRuntimeEnvironment& runtime, headless_cli::ParseResult const& parsed) {
	if (!parsed.command)
		return 64;

	if (auto const* probe = std::get_if<headless_cli::ProbePlaybackCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_playback_probe::PlaybackProbeResult>(
			runtime.MainThreadPump(),
			[request = probe->request](auto&& on_done) mutable {
				aegisub::playback_probe_service::RunAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* session = std::get_if<headless_cli::SessionPlaybackCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_cli::PlaybackSessionResult>(
			runtime.MainThreadPump(),
			[request = session->request](auto&& on_done) mutable {
				headless_cli::RunSessionPlaybackAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* inspect = std::get_if<headless_cli::InspectTraceCommand>(&*parsed.command)) {
		auto result = headless_cli::RunInspectTrace(inspect->request);
		if (!result.output.empty())
			std::cout << result.output;
		if (!result.error.empty())
			std::cerr << result.error << std::endl;
		return result.exit_code;
	}

	if (auto const* inspect = std::get_if<headless_cli::InspectMediaCommand>(&*parsed.command)) {
		auto result = headless_cli::RunInspectMedia(inspect->request);
		std::cout << headless_cli::BuildMediaInspectJson(result);
		if (result.exit_code != 0 && !result.message.empty())
			std::cerr << result.message << std::endl;
		return result.exit_code;
	}

	if (auto const* inspect = std::get_if<headless_cli::InspectAssInfoCommand>(&*parsed.command)) {
		auto result = headless_cli::RunInspectAssInfo(inspect->request);
		std::cout << headless_cli::BuildAssInfoJson(result);
		if (!result.snapshot && !result.error.empty())
			std::cerr << result.error << std::endl;
		return result.snapshot ? 0 : 2;
	}

	if (auto const* batch = std::get_if<headless_cli::BatchPlaybackProbeCommand>(&*parsed.command)) {
		auto result = RunAsyncWithPump<headless_cli::BatchPlaybackProbeResult>(
			runtime.MainThreadPump(),
			[request = batch->request](auto&& on_done) mutable {
				headless_cli::RunBatchPlaybackProbeAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
			});
		return result.exit_code;
	}

	if (auto const* batch = std::get_if<headless_cli::BatchTraceSummarizeCommand>(&*parsed.command)) {
		auto result = headless_cli::RunBatchTraceSummarize(batch->request);
		if (!result.message.empty())
			std::cout << result.message << std::endl;
		return result.exit_code;
	}

	if (auto const* batch = std::get_if<headless_cli::BatchAssInfoCommand>(&*parsed.command)) {
		auto result = headless_cli::RunBatchAssInfo(batch->request);
		if (!result.message.empty())
			std::cout << result.message << std::endl;
		return result.exit_code;
	}

	std::cerr << "unhandled CLI command" << std::endl;
	return 64;
}

int RunParsedLegacyProbe(HeadlessRuntimeEnvironment& runtime, headless_playback_probe::CommandLineParseResult const& parsed) {
	if (!parsed.request)
		return 64;

	auto result = RunAsyncWithPump<headless_playback_probe::PlaybackProbeResult>(
		runtime.MainThreadPump(),
		[request = *parsed.request](auto&& on_done) mutable {
			headless_playback_probe::RunAsync(std::move(request), std::forward<decltype(on_done)>(on_done));
		});
	return result.exit_code;
}

}

bool IsHeadlessCommandLine(std::vector<std::string> const& args) {
	auto const cli_parse = headless_cli::ParseCommandLine(args);
	if (cli_parse.requested)
		return true;
	return headless_playback_probe::ParseCommandLine(args).requested;
}

int RunHeadlessCommandLine(std::vector<std::string> const& args) {
	auto const cli_parse = headless_cli::ParseCommandLine(args);
	if (cli_parse.requested) {
		if (!cli_parse.command) {
			std::cerr << cli_parse.error << std::endl;
			return 64;
		}

		HeadlessRuntimeEnvironment runtime;
		std::string error;
		if (!runtime.Initialize(error)) {
			ReportHeadlessError("headless-init", error);
			return 2;
		}
		return RunParsedHeadlessCli(runtime, cli_parse);
	}

	auto const probe_parse = headless_playback_probe::ParseCommandLine(args);
	if (probe_parse.requested) {
		if (!probe_parse.request) {
			std::cerr << probe_parse.error << std::endl;
			return 64;
		}

		HeadlessRuntimeEnvironment runtime;
		std::string error;
		if (!runtime.Initialize(error)) {
			ReportHeadlessError("headless-init", error);
			return 2;
		}
		return RunParsedLegacyProbe(runtime, probe_parse);
	}

	return 1;
}
