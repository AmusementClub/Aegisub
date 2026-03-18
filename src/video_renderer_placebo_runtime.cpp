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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "video_renderer_placebo_runtime.h"

#include "native_library.h"

#include <libaegisub/exception.h>
#include <libaegisub/log.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define AGI_PL_STRINGIFY_INNER(x) #x
#define AGI_PL_STRINGIFY(x) AGI_PL_STRINGIFY_INNER(x)

namespace placebo { namespace runtime {

namespace {
constexpr char kPlaceboLogTag[] = "video/out/placebo/runtime";
constexpr char kLogCreateSymbol[] = "pl_log_create_" AGI_PL_STRINGIFY(PL_API_VER);

void LogInfo(std::string const& message) {
	if (agi::log::log)
		LOG_I(kPlaceboLogTag) << message;
}

void LogWarning(std::string const& message) {
	if (agi::log::log)
		LOG_W(kPlaceboLogTag) << message;
}

struct RuntimeState {
	std::unique_ptr<agi::native::Library> library;
	Api api;
	std::string load_error;
	std::string loaded_library;
	std::string loaded_version;
	uint32_t loaded_fix_version = 0;
	bool load_attempted = false;
	bool load_complete = false;
};

RuntimeState runtime_state;
std::mutex runtime_mutex;

std::vector<std::string> CandidateLibraryNames() {
	std::vector<std::string> candidates;
	candidates.emplace_back("libplacebo-" AGI_PL_STRINGIFY(PL_API_VER));
	candidates.emplace_back("libplacebo");
	candidates.emplace_back("placebo-" AGI_PL_STRINGIFY(PL_API_VER));
	candidates.emplace_back("placebo");
	return candidates;
}

template <typename T>
void ResolveSymbol(agi::native::Library& library, T& out, char const *name) {
	out = library.ResolveSymbol<T>(name);
}

void ResolveSymbols(agi::native::Library& library, Api& api) {
	ResolveSymbol(library, api.log_create, kLogCreateSymbol);
	ResolveSymbol(library, api.log_destroy, "pl_log_destroy");
	ResolveSymbol(library, api.version, "pl_version");
	ResolveSymbol(library, api.fix_ver, "pl_fix_ver");
	ResolveSymbol(library, api.opengl_create, "pl_opengl_create");
	ResolveSymbol(library, api.opengl_destroy, "pl_opengl_destroy");
	ResolveSymbol(library, api.renderer_create, "pl_renderer_create");
	ResolveSymbol(library, api.renderer_destroy, "pl_renderer_destroy");
	ResolveSymbol(library, api.renderer_flush_cache, "pl_renderer_flush_cache");
	ResolveSymbol(library, api.opengl_wrap, "pl_opengl_wrap");
	ResolveSymbol(library, api.plane_data_from_mask, "pl_plane_data_from_mask");
	ResolveSymbol(library, api.upload_plane, "pl_upload_plane");
	ResolveSymbol(library, api.tex_destroy, "pl_tex_destroy");
	ResolveSymbol(library, api.render_image, "pl_render_image");
}

[[noreturn]] void ThrowCachedLoadError(RuntimeState const& state) {
	throw agi::EnvironmentError(state.load_error.empty()
		? "Failed to load libplacebo runtime."
		: state.load_error);
}

void EnsureLoadedLocked() {
	if (runtime_state.load_complete)
		return;
	if (runtime_state.load_attempted)
		ThrowCachedLoadError(runtime_state);

	runtime_state.load_attempted = true;
	std::string combined_errors;
	for (auto const& candidate : CandidateLibraryNames()) {
		try {
			std::unique_ptr<agi::native::Library> library(new agi::native::Library(agi::native::Library::Load(candidate)));
			ResolveSymbols(*library, runtime_state.api);
			runtime_state.loaded_library = std::string(library->GetLoadedPath());
			runtime_state.loaded_version = runtime_state.api.version ? runtime_state.api.version() : std::string();
			runtime_state.loaded_fix_version = runtime_state.api.fix_ver ? static_cast<uint32_t>(runtime_state.api.fix_ver()) : 0;
			runtime_state.library = std::move(library);
			runtime_state.load_complete = true;
			runtime_state.load_error.clear();

			LogInfo("Loaded libplacebo runtime from " + runtime_state.loaded_library
				+ " (headers API v" + std::to_string(PL_API_VER)
				+ ", runtime " + (runtime_state.loaded_version.empty() ? std::string("unknown") : runtime_state.loaded_version)
				+ ", fix " + std::to_string(runtime_state.loaded_fix_version)
				+ ")");
			return;
		}
		catch (agi::EnvironmentError const& err) {
			if (!combined_errors.empty())
				combined_errors += " | ";
			combined_errors += candidate;
			combined_errors += ": ";
			combined_errors += err.GetMessage();
		}
	}

	runtime_state.load_error = "Could not load a compatible libplacebo runtime. " + combined_errors;
	LogWarning(runtime_state.load_error);
	ThrowCachedLoadError(runtime_state);
}
}

void EnsureLoaded() {
	std::lock_guard<std::mutex> lock(runtime_mutex);
	EnsureLoadedLocked();
}

bool IsAvailable() noexcept {
	try {
		EnsureLoaded();
		return true;
	}
	catch (...) {
		return false;
	}
}

std::string GetLoadError() {
	std::lock_guard<std::mutex> lock(runtime_mutex);
	return runtime_state.load_error;
}

std::string GetLoadedLibrary() {
	std::lock_guard<std::mutex> lock(runtime_mutex);
	return runtime_state.loaded_library;
}

std::string GetLoadedVersion() {
	std::lock_guard<std::mutex> lock(runtime_mutex);
	return runtime_state.loaded_version;
}

uint32_t GetLoadedFixVersion() noexcept {
	std::lock_guard<std::mutex> lock(runtime_mutex);
	return runtime_state.loaded_fix_version;
}

Api const& GetApi() {
	EnsureLoaded();
	return runtime_state.api;
}

} }
