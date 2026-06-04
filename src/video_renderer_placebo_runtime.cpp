// Copyright (c) 2026, MIR

#include "video_renderer_placebo_runtime.h"

#include <libaegisub/native_library.h>

#include <libaegisub/exception.h>

#include <mutex>
#include <string>

#define AGI_PL_STRINGIFY_INNER(x) #x
#define AGI_PL_STRINGIFY(x) AGI_PL_STRINGIFY_INNER(x)

namespace placebo { namespace runtime {

namespace {
constexpr char kPlaceboLogTag[] = "video/out/placebo/runtime";
constexpr char kPlaceboLibraryName[] = "libplacebo";
constexpr char kLogCreateSymbol[] = "pl_log_create_" AGI_PL_STRINGIFY(PL_API_VER);

agi::native::LibraryLoadOptions GetRuntimeLoadOptions() {
	return agi::native::DefaultAppLocalLoadOptions(false);
}

struct RuntimeMetadata {
	Api api;
	std::string loaded_version;
	uint32_t loaded_fix_version = 0;
};

RuntimeMetadata runtime_metadata;
std::mutex runtime_metadata_mutex;

template <typename T>
void ResolveSymbol(agi::native::Library& library, T& out, char const *name) {
	out = library.ResolveSymbol<T>(name);
}

template <typename T>
void TryResolveSymbol(agi::native::Library& library, T& out, char const *name) {
	out = library.TryResolveSymbol<T>(name);
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
	ResolveSymbol(library, api.plane_data_align, "pl_plane_data_align");
	ResolveSymbol(library, api.upload_plane, "pl_upload_plane");
	ResolveSymbol(library, api.tex_destroy, "pl_tex_destroy");
	ResolveSymbol(library, api.frame_set_chroma_location, "pl_frame_set_chroma_location");
	ResolveSymbol(library, api.render_image, "pl_render_image");
	TryResolveSymbol(library, api.hdr_rescale, "pl_hdr_rescale");
	TryResolveSymbol(library, api.hdr_metadata_from_dovi_rpu, "pl_hdr_metadata_from_dovi_rpu");
	TryResolveSymbol(library, api.map_avframe, "pl_map_avframe");
	TryResolveSymbol(library, api.unmap_avframe, "pl_unmap_avframe");

	TryResolveSymbol(library, api.color_space_is_hdr, "pl_color_space_is_hdr");
}

std::string FormatVersionDetailLocked() {
	return "headers API v" + std::to_string(PL_API_VER)
		+ ", runtime " + (runtime_metadata.loaded_version.empty() ? std::string("unknown") : runtime_metadata.loaded_version)
		+ ", fix " + std::to_string(runtime_metadata.loaded_fix_version);
}

std::string GetVersionDetail() {
	std::lock_guard<std::mutex> lock(runtime_metadata_mutex);
	return FormatVersionDetailLocked();
}

std::string FormatCompatibilityLoadError(std::string const& message) {
	if (message.empty())
		return "Could not load a compatible libplacebo runtime '" + std::string(kPlaceboLibraryName) + "'.";
	return "Could not load a compatible libplacebo runtime '" + std::string(kPlaceboLibraryName) + "'. " + message;
}

void InitializeRuntime(agi::native::Library& library) {
	RuntimeMetadata loaded;
	ResolveSymbols(library, loaded.api);
	loaded.loaded_version = loaded.api.version ? loaded.api.version() : std::string();
	loaded.loaded_fix_version = loaded.api.fix_ver ? static_cast<uint32_t>(loaded.api.fix_ver()) : 0;

	std::lock_guard<std::mutex> lock(runtime_metadata_mutex);
	runtime_metadata = std::move(loaded);
}

agi::native::CachedLibrary runtime_library(
	kPlaceboLibraryName,
	"libplacebo runtime",
	kPlaceboLogTag,
	InitializeRuntime,
	GetVersionDetail,
	GetRuntimeLoadOptions());
}

void EnsureLoaded() {
	try {
		runtime_library.EnsureLoaded();
	}
	catch (agi::EnvironmentError const& err) {
		throw agi::EnvironmentError(FormatCompatibilityLoadError(err.GetMessage()));
	}
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
	auto message = runtime_library.GetLoadError();
	return message.empty() ? message : FormatCompatibilityLoadError(message);
}

std::string GetLoadedLibrary() {
	return runtime_library.GetLoadedLibrary();
}

std::string GetLoadedVersion() {
	std::lock_guard<std::mutex> lock(runtime_metadata_mutex);
	return runtime_metadata.loaded_version;
}

uint32_t GetLoadedFixVersion() noexcept {
	std::lock_guard<std::mutex> lock(runtime_metadata_mutex);
	return runtime_metadata.loaded_fix_version;
}

Api const& GetApi() {
	EnsureLoaded();
	return runtime_metadata.api;
}

} }
