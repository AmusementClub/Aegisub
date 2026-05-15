#include "scenechange_native_api.h"

#include <libaegisub/exception.h>
#include <libaegisub/native_library.h>

#include <mutex>
#include <string>

namespace scenechange {
namespace {

constexpr char kLogTag[] = "provider/scenechange/runtime";
constexpr char kLibraryName[] = "scenechange_native";

Api api;
std::mutex api_mutex;

template <typename T>
void ResolveSymbol(agi::native::Library& library, T& out, char const *name) {
	out = library.ResolveSymbol<T>(name);
}

template <typename T>
void TryResolveSymbol(agi::native::Library& library, T& out, char const *name) {
	out = library.TryResolveSymbol<T>(name);
}

void ResolveSymbols(agi::native::Library& library, Api& loaded) {
	ResolveSymbol(library, loaded.get_api_version, "scenechange_get_api_version");
	TryResolveSymbol(library, loaded.get_versions_json_utf8, "scenechange_get_versions_json_utf8");
	TryResolveSymbol(library, loaded.free, "scenechange_free");
	ResolveSymbol(library, loaded.wwxd_create, "scenechange_wwxd_create");
	ResolveSymbol(library, loaded.wwxd_destroy, "scenechange_wwxd_destroy");
	ResolveSymbol(library, loaded.wwxd_set_progress_callback, "scenechange_wwxd_set_progress_callback");
	ResolveSymbol(library, loaded.wwxd_get_write_buffer, "scenechange_wwxd_get_write_buffer");
	ResolveSymbol(library, loaded.wwxd_commit_written_frame_no_pad, "scenechange_wwxd_commit_written_frame_no_pad");
}

std::string FormatApiVersion(int32_t version) {
	return std::to_string((version >> 16) & 0xff) + "."
		+ std::to_string((version >> 8) & 0xff) + "."
		+ std::to_string(version & 0xff);
}

void ValidateApiVersion(Api const& loaded) {
	int32_t const actual = loaded.get_api_version();
	if (actual == kApiVersion)
		return;

	throw agi::EnvironmentError(
		"SceneChange API version mismatch: loaded " + FormatApiVersion(actual)
		+ " (" + std::to_string(actual) + "), expected "
		+ FormatApiVersion(kApiVersion)
		+ " (" + std::to_string(kApiVersion) + ").");
}

std::string GetRuntimeVersionDetail() {
	Api loaded;
	{
		std::lock_guard<std::mutex> lock(api_mutex);
		loaded = api;
	}

	if (!loaded.get_api_version)
		return {};

	std::string detail = "api=" + FormatApiVersion(loaded.get_api_version())
		+ ", expected-api=" + FormatApiVersion(kApiVersion);

	if (!loaded.get_versions_json_utf8 || !loaded.free)
		return detail + ", versions unavailable";

	char *error = nullptr;
	char *versions = loaded.get_versions_json_utf8(&error);
	if (versions && *versions)
		detail += ", versions=" + std::string(versions);
	else if (error && *error)
		detail += ", versions-error=" + std::string(error);
	else
		detail += ", versions unavailable";

	if (versions)
		loaded.free(versions);
	if (error)
		loaded.free(error);

	return detail;
}

void InitializeRuntime(agi::native::Library& library) {
	Api loaded;
	ResolveSymbols(library, loaded);
	ValidateApiVersion(loaded);

	std::lock_guard<std::mutex> lock(api_mutex);
	api = loaded;
}

agi::native::CachedLibrary runtime_library(
	kLibraryName,
	"SceneChange runtime",
	kLogTag,
	InitializeRuntime,
	GetRuntimeVersionDetail,
	agi::native::DefaultAppLocalLoadOptions(false));

std::string FormatLoadError(std::string const& message) {
	if (message.empty())
		return "Could not load SceneChange runtime '" + std::string(kLibraryName) + "'.";
	return "Could not load SceneChange runtime '" + std::string(kLibraryName) + "'. " + message;
}

} // namespace

void EnsureLoaded() {
	try {
		runtime_library.EnsureLoaded();
	}
	catch (agi::EnvironmentError const& err) {
		throw agi::EnvironmentError(FormatLoadError(err.GetMessage()));
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
	return message.empty() ? message : FormatLoadError(message);
}

std::string GetLoadedLibrary() {
	return runtime_library.GetLoadedLibrary();
}

Api const& GetApi() {
	EnsureLoaded();
	return api;
}

} // namespace scenechange
