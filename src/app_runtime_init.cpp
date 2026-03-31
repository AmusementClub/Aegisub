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

#include "app_runtime_init.h"

#include "app_runtime.h"

#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "crash_writer.h"
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "subtitles_provider_libass.h"
#include "utils.h"
#include "version.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>

#include <boost/interprocess/streams/bufferstream.hpp>

#include <istream>
#include <memory>

void InitializeRuntimePathsAndOptions() {
	config::path = new agi::Path;
	crash_writer::Initialize(config::path->Decode("?user"));

	if (!agi::log::log) {
		agi::log::log = new agi::log::LogSink;
#ifdef _DEBUG
		agi::log::log->Subscribe(agi::make_unique<agi::log::EmitSTDOUT>());
#endif
	}

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

	if (!config::opt)
		config::opt = new agi::Options(config::path->Decode("?user/config.json"), GET_DEFAULT_CONFIG(default_config));

	boost::interprocess::ibufferstream stream((const char *)default_config_platform, sizeof(default_config_platform));
	config::opt->ConfigNext(stream);

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

	config::mru = new agi::MRUManager(config::path->Decode("?user/mru.json"), GET_DEFAULT_CONFIG(default_mru), config::opt);
}

void InitializeRuntimeLoggingAndPerfTrace() {
	perf_trace::Initialize(GetAegisubLongVersionString());
	auto path_log = config::path->Decode("?user/log/");
	agi::fs::CreateDirectory(path_log);
	agi::log::log->Subscribe(agi::make_unique<agi::log::JsonEmitter>(path_log));
	CleanCache(path_log, "*.ndjson", 10, 100, 24 * 60 * 60);
	CleanCache(path_log, "*.json", 10, 100, 24 * 60 * 60);
}

void InitializeOptionalRuntimeFacilities(AppRuntimeInitOptions const& options) {
	if (options.register_automation_script_factory)
		Automation4::ScriptFactory::Register(agi::make_unique<Automation4::LuaScriptFactory>());

	if (options.warm_subtitles_provider_font_cache)
		libass::CacheFonts();

	if (options.load_global_scripts)
		config::global_scripts = new Automation4::AutoloadScriptManager(OPT_GET("Path/Automation/Autoload")->GetString());

	if (options.register_export_filters) {
		AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
		AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());
	}

	if (options.install_png_handler) {
		if (!options.host_hooks.install_png_image_handler)
			throw agi::InternalError("AppRuntime requested PNG handler installation without a host hook.");
		options.host_hooks.install_png_image_handler();
	}
}

void CleanupRuntimeProcessState() {
	if (config::opt) {
		delete config::opt;
		config::opt = nullptr;
	}
	if (config::mru) {
		delete config::mru;
		config::mru = nullptr;
	}
	if (config::global_scripts) {
		delete config::global_scripts;
		config::global_scripts = nullptr;
	}

	AssExportFilterChain::Clear();

	perf_trace::Shutdown();

	if (agi::log::log) {
		auto* sink = agi::log::log;
		agi::log::log = nullptr;
		delete sink;
	}

	// Legacy startup/shutdown left config::path alive until process teardown.
	// Keep the same lifetime for now so late cleanup paths do not dereference
	// a freed process-global path object.

	crash_writer::Cleanup();
}
