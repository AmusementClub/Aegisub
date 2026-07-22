#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Synchronized with SceneChangeSharp common provider ABI.
#include "scenechange_provider_api.h"

#if defined(_MSC_VER)
#define SCENECHANGE_NATIVE_CALL __cdecl
#else
#define SCENECHANGE_NATIVE_CALL
#endif

extern "C" {

using scenechange_progress_callback_fn = int32_t (SCENECHANGE_NATIVE_CALL *)(
	void *user_data,
	int32_t processed_frames,
	int32_t scene_change);
}

namespace scenechange {

constexpr int32_t kApiVersion = 0x00010000;

struct Api {
	enum class Backend {
		None,
		WwxdProvider,
		ScxvidProvider,
	};

	Backend backend = Backend::None;
	scenechange_provider_api provider {};
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
Api const& GetApi();
std::string GetBackendName();
std::string GetCacheToken();
// Selects the preferred compatible backend for the current video dimensions.
bool SupportsDimensions(int32_t width, int32_t height) noexcept;

} // namespace scenechange
