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
#include "options.h"

#include <libaegisub/log.h>

#ifdef _WIN32
#include <libaegisub/charset_conv_win.h>
#include <libaegisub/util.h>
#endif

#include <mutex>
#include <string>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifndef AVISYNTH_SO
// Fallback definition
#ifdef _WIN32
#define AVISYNTH_SO "AviSynth.dll"
#else
#define AVISYNTH_SO "libavisynth.so"
#endif
#endif

const AVS_Linkage* AVS_linkage;

typedef IScriptEnvironment* __stdcall FUNC(int);

// Allocate storage for and initialise static members
namespace {
	int avs_refcount = 0;
#ifdef _WIN32
	HINSTANCE hLib = nullptr;
#else
	void* hLib = nullptr;
#endif
	IScriptEnvironment *env = nullptr;
	std::mutex AviSynthMutex;
	FUNC* CreateScriptEnv = nullptr;
	std::string load_error;
	std::string loaded_library;
	bool load_attempted = false;
	bool load_complete = false;

	std::string GetLoadFailureReason() {
#ifdef _WIN32
		return agi::util::ErrorString(GetLastError());
#else
		auto err = dlerror();
		return err ? err : "unknown error";
#endif
	}

	std::string GetLoadedLibraryPath() {
#ifdef _WIN32
		std::wstring path(32768, L'\0');
		auto len = GetModuleFileNameW(hLib, &path[0], static_cast<DWORD>(path.size()));
		if (!len)
			return AVISYNTH_SO;
		path.resize(len);
		return agi::charset::ConvertW(path);
#else
		Dl_info info{};
		if (CreateScriptEnv && dladdr(reinterpret_cast<void *>(CreateScriptEnv), &info) && info.dli_fname)
			return info.dli_fname;
		return AVISYNTH_SO;
#endif
	}

	void EnsureAvisynthRuntimeLoaded() {
		if (load_complete)
			return;
		if (load_attempted)
			throw AvisynthError(load_error.c_str());
		load_attempted = true;

#ifdef _WIN32
#define CONCATENATE(x, y) x ## y
#define _Lstr(x) CONCATENATE(L, x)
		hLib = LoadLibraryW(_Lstr(AVISYNTH_SO));
#undef _Lstr
#undef CONCATENATE
#else
		hLib = dlopen(AVISYNTH_SO, RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
#endif

		if (!hLib) {
			load_error = std::string("Could not load Avisynth runtime library '") + AVISYNTH_SO + "': " + GetLoadFailureReason();
			LOG_W("provider/avisynth/runtime") << load_error;
			throw AvisynthError(load_error.c_str());
		}

#ifdef _WIN32
		CreateScriptEnv = reinterpret_cast<FUNC*>(GetProcAddress(hLib, "CreateScriptEnvironment"));
#else
		dlerror();
		CreateScriptEnv = reinterpret_cast<FUNC*>(dlsym(hLib, "CreateScriptEnvironment"));
#endif
		if (!CreateScriptEnv) {
			load_error = std::string("Failed to resolve Avisynth symbol CreateScriptEnvironment: ") + GetLoadFailureReason();
			LOG_W("provider/avisynth/runtime") << load_error;
			throw AvisynthError(load_error.c_str());
		}

		loaded_library = GetLoadedLibraryPath();
		load_error.clear();
		load_complete = true;
		LOG_I("provider/avisynth/runtime") << "Loaded Avisynth from " << loaded_library;
	}
}

namespace avisynth {
	bool IsAvailable() noexcept {
		std::lock_guard<std::mutex> lock(AviSynthMutex);
		try {
			EnsureAvisynthRuntimeLoaded();
			return true;
		}
		catch (...) {
			return false;
		}
	}

	std::string GetLoadError() {
		std::lock_guard<std::mutex> lock(AviSynthMutex);
		return load_error;
	}

	std::string GetLoadedLibrary() {
		std::lock_guard<std::mutex> lock(AviSynthMutex);
		return loaded_library;
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
	}
}

std::mutex& AviSynthWrapper::GetMutex() const {
	return AviSynthMutex;
}

IScriptEnvironment *AviSynthWrapper::GetEnv() const {
	return env;
}

#endif
