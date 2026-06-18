// Copyright (c) 2026, MIR

#pragma once

#include <libaegisub/native_library.h>

#include <functional>
#include <string>
#include <string_view>

#ifndef AVISYNTH_SO
#ifdef _WIN32
#define AVISYNTH_SO "AviSynth.dll"
#else
#define AVISYNTH_SO "libavisynth.so"
#endif
#endif

namespace avisynth {

struct RuntimeLoadRequest {
	std::string library_name;
	agi::native::LibraryLoadOptions load_options;

	bool operator==(RuntimeLoadRequest const&) const = default;
};

inline std::string GetDefaultRuntimeLibraryName() {
	return AVISYNTH_SO;
}

using RuntimePathResolver = std::function<std::string(std::string_view)>;

inline bool LooksLikeTokenPath(std::string_view path) {
	return !path.empty() && path[0] == '?';
}

inline std::string ResolveConfiguredRuntimePath(std::string_view configured_runtime_path, RuntimePathResolver const& path_resolver = RuntimePathResolver()) {
	if (configured_runtime_path.empty())
		return {};

	auto configured_path = std::string(configured_runtime_path);
	auto resolved_path = path_resolver ? path_resolver(configured_runtime_path) : configured_path;
	if (LooksLikeTokenPath(configured_runtime_path) && LooksLikeTokenPath(resolved_path))
		return {};
	return resolved_path;
}

inline bool UsesAppLocalRuntime(std::string_view configured_runtime_path, RuntimePathResolver const& path_resolver = RuntimePathResolver()) {
	return ResolveConfiguredRuntimePath(configured_runtime_path, path_resolver).empty();
}

inline RuntimeLoadRequest BuildRuntimeLoadRequest(std::string_view configured_runtime_path, RuntimePathResolver const& path_resolver = RuntimePathResolver()) {
	RuntimeLoadRequest request;
	auto resolved_path = ResolveConfiguredRuntimePath(configured_runtime_path, path_resolver);
	request.library_name = resolved_path.empty()
		? GetDefaultRuntimeLibraryName()
		: std::move(resolved_path);
	request.load_options = agi::native::DefaultAppLocalLoadOptions(false);
	return request;
}

}
