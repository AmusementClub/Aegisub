// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file avisynth_wrap.cpp
/// @brief Wrapper-layer for Avisynth
/// @ingroup video_input audio_input
///

#ifdef WITH_AVISYNTH
#include "avisynth_wrap.h"

#include <avisynth.h>
#include "avisynth_path_helper.h"
#include "avisynth_runtime_policy.h"
#include "native_library.h"
#include "options.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

const AVS_Linkage* AVS_linkage;

typedef IScriptEnvironment* __stdcall FUNC(int);

// Allocate storage for and initialise static members
namespace {
	constexpr char kAvisynthPluginLogTag[] = "provider/avisynth/plugins";
	int avs_refcount = 0;
	IScriptEnvironment *env = nullptr;
	std::mutex AviSynthMutex;
	FUNC* CreateScriptEnv = nullptr;
	std::unique_ptr<agi::native::CachedLibrary> runtime_library;
	avisynth::RuntimeLoadRequest runtime_request;
	bool plugin_autoload_dirs_configured = false;

	bool UsesAppLocalPluginLoading() {
		return avisynth::UsesAppLocalRuntime(OPT_GET("Provider/Avisynth/Runtime Path")->GetString());
	}

	void InitializeAvisynthRuntime(agi::native::Library& library) {
		CreateScriptEnv = library.ResolveSymbol<FUNC*>("CreateScriptEnvironment");
	}

	avisynth::RuntimeLoadRequest GetConfiguredRuntimeRequest() {
		return avisynth::BuildRuntimeLoadRequest(
			OPT_GET("Provider/Avisynth/Runtime Path")->GetString(),
			[](std::string_view configured_runtime_path) {
				return config::path
					? agi::fs::PathToString(config::path->Decode(std::string(configured_runtime_path)))
					: std::string(configured_runtime_path);
			});
	}

	agi::native::CachedLibrary& GetRuntimeLibrary() {
		auto requested = GetConfiguredRuntimeRequest();
		// The Avisynth environment is process-global in this wrapper, so only
		// switch runtimes when no clip is actively holding the current one.
		if (!runtime_library || (avs_refcount == 0 && requested != runtime_request)) {
			runtime_library = std::make_unique<agi::native::CachedLibrary>(
				requested.library_name,
				"Avisynth",
				"provider/avisynth/runtime",
				InitializeAvisynthRuntime,
				agi::native::CachedLibrary::DetailFunction(),
				requested.load_options);
			runtime_request = std::move(requested);
		}
		return *runtime_library;
	}

	std::vector<agi::fs::path> GetAppPluginAutoloadDirectories() {
		std::vector<agi::fs::path> directories;
		if (!config::path)
			return directories;

		auto append_if_exists = [&](char const *token_path) {
			auto path = config::path->Decode(token_path);
			if (agi::fs::DirectoryExists(path))
				directories.push_back(std::move(path));
		};

		append_if_exists("?user/runtimes/avs-plugins");
		append_if_exists("?data/runtimes/avs-plugins");
		return directories;
	}

	bool TryRegisterLegacyAutoloadDir(PNeoEnv const& neo_env, agi::fs::path const& path) {
		auto legacy_path = avisynth::TryGetLegacyPathString(path);
		if (!legacy_path)
			return false;

		neo_env->AddAutoloadDir(env->SaveString(legacy_path->c_str()), true);
		return true;
	}

	void ConfigurePluginAutoloadDirectories() {
		if (plugin_autoload_dirs_configured || !env)
			return;
		plugin_autoload_dirs_configured = true;

		if (!UsesAppLocalPluginLoading()) {
			LOG_D(kAvisynthPluginLogTag) << "Skipping app-local Avisynth plugin autoload because an explicit runtime path is configured.";
			return;
		}

		PNeoEnv neo_env(env);
		if (!neo_env) {
			LOG_D(kAvisynthPluginLogTag) << "Avisynth runtime does not expose INeoEnv autoload APIs; keeping explicit plugin fallback only.";
			return;
		}

		auto directories = GetAppPluginAutoloadDirectories();
		if (directories.empty()) {
			LOG_D(kAvisynthPluginLogTag) << "No app-local Avisynth autoload directories found.";
			return;
		}

		for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
			LOG_I(kAvisynthPluginLogTag) << "Registering Avisynth autoload dir: " << agi::fs::PathToString(*it);
			try {
				avisynth::InvokeUtf8PathFunction(env, "AddAutoloadDir", *it, { true });
			}
			catch (AvisynthError const& err) {
				LOG_D(kAvisynthPluginLogTag) << "Avisynth script AddAutoloadDir fallback failed for " << agi::fs::PathToString(*it) << ": " << err.msg;
				try {
					if (!TryRegisterLegacyAutoloadDir(neo_env, *it)) {
						LOG_W(kAvisynthPluginLogTag) << "Skipping legacy Avisynth AddAutoloadDir fallback for " << agi::fs::PathToString(*it) << " because the path is not ANSI/8.3-safe.";
					}
				}
				catch (AvisynthError const& neo_err) {
					LOG_W(kAvisynthPluginLogTag) << "Avisynth INeoEnv AddAutoloadDir failed for " << agi::fs::PathToString(*it) << ": " << neo_err.msg;
				}
			}
		}
		neo_env->AutoloadPlugins();

		if (char *dirs = neo_env->ListAutoloadDirs(); dirs && *dirs)
			LOG_I(kAvisynthPluginLogTag) << "Avisynth autoload dirs: " << dirs;
	}

	bool TryLoadPluginFallback(char const *token_path) {
		if (!env)
			return false;
		auto path = config::path ? config::path->Decode(token_path) : agi::fs::path(token_path);
		if (!agi::fs::FileExists(path))
			return false;

		try {
			LOG_I(kAvisynthPluginLogTag) << "Falling back to explicit Avisynth LoadPlugin for " << agi::fs::PathToString(path);
			avisynth::InvokeUtf8PathFunction(env, "LoadPlugin", path);
			return true;
		}
		catch (AvisynthError const& err) {
			LOG_W(kAvisynthPluginLogTag) << "Avisynth LoadPlugin failed for " << agi::fs::PathToString(path) << ": " << err.msg;
			return false;
		}
	}

	void EnsureAvisynthRuntimeLoaded() {
		try {
			GetRuntimeLibrary().EnsureLoaded();
		}
		catch (agi::EnvironmentError const& err) {
			throw AvisynthError(err.GetMessage().c_str());
		}
	}
}

namespace avisynth {
	bool IsAvailable() noexcept {
		std::lock_guard<std::mutex> lock(AviSynthMutex);
		return GetRuntimeLibrary().IsAvailable();
	}

	std::string GetLoadError() {
		std::lock_guard<std::mutex> lock(AviSynthMutex);
		return GetRuntimeLibrary().GetLoadError();
	}

	std::string GetLoadedLibrary() {
		std::lock_guard<std::mutex> lock(AviSynthMutex);
		return GetRuntimeLibrary().GetLoadedLibrary();
	}
}

AviSynthWrapper::AviSynthWrapper() {
	std::lock_guard<std::mutex> lock(AviSynthMutex);
	EnsureAvisynthRuntimeLoaded();

	if (!avs_refcount++) {
		// Require Avisynth 2.5.6+?
		if (OPT_GET("Provider/Avisynth/Allow Ancient")->GetBool())
			env = CreateScriptEnv(AVISYNTH_INTERFACE_VERSION-1);
		else
			env = CreateScriptEnv(AVISYNTH_INTERFACE_VERSION);

		if (!env)
			throw AvisynthError("Failed to create a new avisynth script environment. Avisynth is too old?");
		AVS_linkage = env->GetAVSLinkage();
		ConfigurePluginAutoloadDirectories();

		// Set memory limit
		const int memoryMax = OPT_GET("Provider/Avisynth/Memory Max")->GetInt();
		if (memoryMax)
			env->SetMemoryMax(memoryMax);
	}
}

AviSynthWrapper::~AviSynthWrapper() {
	std::lock_guard<std::mutex> lock(AviSynthMutex);
	if (!--avs_refcount) {
		delete env;
		env = nullptr;
		if (runtime_library)
			runtime_library->Reset();
		plugin_autoload_dirs_configured = false;
		CreateScriptEnv = nullptr;
	}
}

std::mutex& AviSynthWrapper::GetMutex() const {
	return AviSynthMutex;
}

IScriptEnvironment *AviSynthWrapper::GetEnv() const {
	return env;
}

bool AviSynthWrapper::EnsurePluginLoaded(char const *function_name, std::initializer_list<char const *> candidate_token_paths) const {
	if (!env || !function_name || !*function_name)
		return false;
	if (env->FunctionExists(function_name))
		return true;

	ConfigurePluginAutoloadDirectories();
	if (env->FunctionExists(function_name))
		return true;
	if (!UsesAppLocalPluginLoading())
		return false;

	for (auto const *candidate : candidate_token_paths) {
		if (!candidate || !*candidate)
			continue;
		if (TryLoadPluginFallback(candidate) && env->FunctionExists(function_name))
			return true;
	}

	return env->FunctionExists(function_name);
}

#endif
