#include "lsmas_native_api.h"

#include <libaegisub/exception.h>
#include <libaegisub/native_library.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace lsmas {
namespace {
constexpr char kLogTag[] = "provider/lsmasnative/runtime";
constexpr char kLibraryName[] = "lsmasnative";

Api api;
std::mutex api_mutex;

agi::native::LibraryLoadOptions GetRuntimeLoadOptions() {
    return agi::native::DefaultAppLocalLoadOptions(false);
}

template <typename T>
void ResolveSymbol(agi::native::Library& library, T& out, char const *name) {
    out = library.ResolveSymbol<T>(name);
}

template <typename T>
void TryResolveSymbol(agi::native::Library& library, T& out, char const *name) {
    out = library.TryResolveSymbol<T>(name);
}

void ResolveSymbols(agi::native::Library& library, Api& loaded) {
#define AGI_LSMAS_REQUIRED(symbol, member) ResolveSymbol(library, loaded.member, #symbol);
#define AGI_LSMAS_OPTIONAL(symbol, member) TryResolveSymbol(library, loaded.member, #symbol);
#include "lsmas_native_api.functions.inc"
#undef AGI_LSMAS_OPTIONAL
#undef AGI_LSMAS_REQUIRED
}

std::string FormatApiVersion(int32_t version) {
    return std::to_string((version >> 16) & 0xff) + "."
        + std::to_string((version >> 8) & 0xff) + "."
        + std::to_string(version & 0xff);
}

int32_t ValidateApiVersion(Api const& loaded) {
    int32_t const actual = loaded.get_api_version();
    if (IsSupportedApiVersion(actual))
        return actual;

    throw agi::EnvironmentError(
        "LsmasNative API version unsupported: loaded " + FormatApiVersion(actual)
        + " (" + std::to_string(actual) + "), requires API "
        + std::to_string(LSMAS_NATIVE_API_VERSION_MAJOR) + ".x (>= "
        + FormatApiVersion(api_version::kMinimumCompatible) + ").");
}

std::string GetRuntimeVersionDetail() {
    Api loaded;
    {
        std::lock_guard<std::mutex> lock(api_mutex);
        loaded = api;
    }

    if (!loaded.get_api_version || !loaded.get_versions_json_utf8 || !loaded.free)
        return {};

    std::string detail = "api=" + FormatApiVersion(loaded.api_version)
        + ", minimum-api=" + FormatApiVersion(api_version::kMinimumCompatible);

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
    loaded.api_version = ValidateApiVersion(loaded);

    std::lock_guard<std::mutex> lock(api_mutex);
    api = loaded;
}

agi::native::CachedLibrary runtime_library(
    kLibraryName,
    "LsmasNative runtime",
    kLogTag,
    InitializeRuntime,
    GetRuntimeVersionDetail,
    GetRuntimeLoadOptions());

std::string FormatLoadError(std::string const& message) {
    if (message.empty())
        return "Could not load LsmasNative runtime '" + std::string(kLibraryName) + "'.";
    return "Could not load LsmasNative runtime '" + std::string(kLibraryName) + "'. " + message;
}
}

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
}
