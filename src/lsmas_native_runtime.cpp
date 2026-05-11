#include "lsmas_native_api.h"

#include <libaegisub/exception.h>
#include <libaegisub/native_library.h>

#include <mutex>

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

void ResolveSymbols(agi::native::Library& library, Api& loaded) {
    ResolveSymbol(library, loaded.probe_streams_json_utf8, "lsmas_probe_streams_json_utf8");
    ResolveSymbol(library, loaded.get_versions_json_utf8, "lsmas_get_versions_json_utf8");
    ResolveSymbol(library, loaded.video_open_with_progress_utf8, "lsmas_video_open_with_progress_utf8");
    ResolveSymbol(library, loaded.video_close, "lsmas_video_close");
    ResolveSymbol(library, loaded.video_get_info, "lsmas_video_get_info");
    ResolveSymbol(library, loaded.video_get_stream_props, "lsmas_video_get_stream_props");
    ResolveSymbol(library, loaded.video_get_time_base, "lsmas_video_get_time_base");
    ResolveSymbol(library, loaded.video_get_pts_list, "lsmas_video_get_pts_list");
    ResolveSymbol(library, loaded.video_get_source_frame_count, "lsmas_video_get_source_frame_count");
    ResolveSymbol(library, loaded.video_get_source_keyframe_flags, "lsmas_video_get_source_keyframe_flags");
    ResolveSymbol(library, loaded.video_get_frame_bgra, "lsmas_video_get_frame_bgra");
    ResolveSymbol(library, loaded.video_acquire_avframe, "lsmas_video_acquire_avframe");
    ResolveSymbol(library, loaded.video_frame_get_avframe, "lsmas_video_frame_get_avframe");
    ResolveSymbol(library, loaded.video_frame_get_pix_fmt_name, "lsmas_video_frame_get_pix_fmt_name");
    ResolveSymbol(library, loaded.video_frame_get_props, "lsmas_video_frame_get_props");
    ResolveSymbol(library, loaded.video_frame_get_format_info, "lsmas_video_frame_get_format_info");
    ResolveSymbol(library, loaded.video_frame_get_plane, "lsmas_video_frame_get_plane");
    ResolveSymbol(library, loaded.video_frame_get_side_data, "lsmas_video_frame_get_side_data");
    ResolveSymbol(library, loaded.video_release_frame, "lsmas_video_release_frame");
    ResolveSymbol(library, loaded.audio_open_with_progress_utf8, "lsmas_audio_open_with_progress_utf8");
    ResolveSymbol(library, loaded.audio_close, "lsmas_audio_close");
    ResolveSymbol(library, loaded.audio_get_info, "lsmas_audio_get_info");
    ResolveSymbol(library, loaded.audio_get_samples, "lsmas_audio_get_samples");
    ResolveSymbol(library, loaded.free, "lsmas_free");
}

void InitializeRuntime(agi::native::Library& library) {
    Api loaded;
    ResolveSymbols(library, loaded);

    std::lock_guard<std::mutex> lock(api_mutex);
    api = loaded;
}

agi::native::CachedLibrary runtime_library(
    kLibraryName,
    "LsmasNative runtime",
    kLogTag,
    InitializeRuntime,
    agi::native::CachedLibrary::DetailFunction(),
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
