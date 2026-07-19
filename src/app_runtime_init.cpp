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
#include "app_runtime_facilities.h"

#include "crash_writer.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "utils.h"
#include "version.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>

#include <boost/interprocess/streams/bufferstream.hpp>

#include <istream>
#include <memory>

void InitializeRuntimePathsAndOptions(AppRuntimeInitOptions const& options) {
	config::path = new agi::Path;
	if (options.path_overrides.user_directory)
		config::path->SetToken("?user", *options.path_overrides.user_directory);
	if (options.path_overrides.local_directory)
		config::path->SetToken("?local", *options.path_overrides.local_directory);
	crash_writer::Initialize(config::path->Decode("?user"));

	if (!agi::log::log) {
		agi::log::log = new agi::log::LogSink;
#ifdef _DEBUG
		agi::log::log->Subscribe(agi::make_unique<agi::log::EmitSTDOUT>());
#endif
	}

#ifdef __WXMSW__
	if (options.path_overrides.allow_portable_config) {
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

void CleanupRuntimeProcessState() {
	if (config::opt) {
		delete config::opt;
		config::opt = nullptr;
	}
	if (config::mru) {
		delete config::mru;
		config::mru = nullptr;
	}

	CleanupRuntimeOptionalFacilities();

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
