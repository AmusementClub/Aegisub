#pragma once

#include "lsmas_native_api.generated.h"

#include <string>

namespace lsmas {

namespace api_version {

// API 1.0 provides the legacy frame-output API. API 1.1 adds YUV420P8
// caller-buffer output for SceneChange.
constexpr int32_t kMinimumCompatible = LSMAS_NATIVE_MAKE_API_VERSION(1, 0, 0);
constexpr int32_t kYuv420p8Output = LSMAS_NATIVE_MAKE_API_VERSION(1, 1, 0);

} // namespace api_version

// Feature gates must stay within the vendored header's declared API surface.
static_assert(LSMAS_NATIVE_API_VERSION >= api_version::kYuv420p8Output,
    "vendored lsmasnative headers are older than the highest feature gate");

constexpr bool IsApiVersionAtLeast(int32_t actual, int32_t required) noexcept {
    return actual >= required;
}

// Accept same-major versions at or above the minimum. Cross-major DLLs may
// rearrange ABI layouts (for example lsmas_video_frame_buffer_layout_t).
constexpr bool IsSupportedApiVersion(int32_t actual) noexcept {
    return IsApiVersionAtLeast(actual, api_version::kMinimumCompatible)
        && ((actual >> 16) & 0xff) == LSMAS_NATIVE_API_VERSION_MAJOR;
}

// output_format is int32_t so out-of-range values can be rejected without
// converting through an unscoped enum (which is UB for values outside the
// enumeration range).
constexpr bool SupportsVideoFrameOutput(int32_t actual_version, int32_t output_format) noexcept {
    if (!IsSupportedApiVersion(actual_version))
        return false;

    switch (output_format) {
    case LSMAS_VIDEO_FRAME_OUTPUT_NATIVE:
    case LSMAS_VIDEO_FRAME_OUTPUT_GRAY8:
    case LSMAS_VIDEO_FRAME_OUTPUT_BGRA:
    case LSMAS_VIDEO_FRAME_OUTPUT_RGBA:
    case LSMAS_VIDEO_FRAME_OUTPUT_GRAY8_PADDED16:
        return true;
    case LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8:
        return IsApiVersionAtLeast(actual_version, api_version::kYuv420p8Output);
    default:
        return false;
    }
}

struct Api {
    int32_t api_version = 0;
#define AGI_LSMAS_REQUIRED(symbol, member) decltype(&symbol) member = nullptr;
#define AGI_LSMAS_OPTIONAL(symbol, member) decltype(&symbol) member = nullptr;
#include "lsmas_native_api.functions.inc"
#undef AGI_LSMAS_OPTIONAL
#undef AGI_LSMAS_REQUIRED
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
Api const& GetApi();

}
