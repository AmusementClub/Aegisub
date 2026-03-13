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
#include "native_library.h"
#include "options.h"

#include <libaegisub/log.h>

#include <mutex>
#include <memory>
#include <string>

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
	IScriptEnvironment *env = nullptr;
	std::mutex AviSynthMutex;
	FUNC* CreateScriptEnv = nullptr;

	std::string GetLibraryName() {
#ifdef AVISYNTH_SO
		return AVISYNTH_SO;
#else
		return "AviSynth";
#endif
	}

	void InitializeAvisynthRuntime(agi::native::Library& library) {
		CreateScriptEnv = library.ResolveSymbol<FUNC*>("CreateScriptEnvironment");
	}

	agi::native::CachedLibrary runtime_library(GetLibraryName(), "Avisynth", "provider/avisynth/runtime",
		InitializeAvisynthRuntime);

	void EnsureAvisynthRuntimeLoaded() {
		try {
			runtime_library.EnsureLoaded();
		}
		catch (agi::EnvironmentError const& err) {
			throw AvisynthError(err.GetMessage().c_str());
		}
	}
}

namespace avisynth {
	bool IsAvailable() noexcept {
		return runtime_library.IsAvailable();
	}

	std::string GetLoadError() {
		return runtime_library.GetLoadError();
	}

	std::string GetLoadedLibrary() {
		return runtime_library.GetLoadedLibrary();
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
		runtime_library.Reset();
		CreateScriptEnv = nullptr;
	}
}

std::mutex& AviSynthWrapper::GetMutex() const {
	return AviSynthMutex;
}

IScriptEnvironment *AviSynthWrapper::GetEnv() const {
	return env;
}

#endif
