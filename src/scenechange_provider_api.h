#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define SCENECHANGE_PROVIDER_CALL __cdecl
#else
#  define SCENECHANGE_PROVIDER_CALL
#endif

#ifndef SCENECHANGE_PROVIDER_EXPORT
#  define SCENECHANGE_PROVIDER_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCENECHANGE_PROVIDER_API_VERSION 0x00010000u

/*
 * For every structure containing struct_size, the caller supplies its writable
 * capacity on input. On success the provider replaces it with the V1 prefix
 * size it initialized and leaves any bytes beyond that prefix untouched.
 */

enum scenechange_provider_status {
    SCENECHANGE_PROVIDER_OK = 0,
    SCENECHANGE_PROVIDER_CANCELED = 1,
    SCENECHANGE_PROVIDER_INVALID_ARGUMENT = -1,
    SCENECHANGE_PROVIDER_INITIALIZATION_FAILED = -2,
    SCENECHANGE_PROVIDER_PROCESS_FAILED = -3,
    SCENECHANGE_PROVIDER_INTERNAL_ERROR = -4,
    SCENECHANGE_PROVIDER_UNSUPPORTED = -5
};

enum scenechange_provider_backend {
    SCENECHANGE_PROVIDER_BACKEND_UNKNOWN = 0,
    SCENECHANGE_PROVIDER_BACKEND_WWXD = 1,
    SCENECHANGE_PROVIDER_BACKEND_SCXVID = 2,
    SCENECHANGE_PROVIDER_BACKEND_HYBRID = 3
};

enum scenechange_provider_pixel_format {
    SCENECHANGE_PROVIDER_PIXEL_FORMAT_UNKNOWN = 0,
    SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16 = 1,
    SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8 = 2
};

enum scenechange_provider_transition_kind {
    SCENECHANGE_PROVIDER_TRANSITION_NONE = 0,
    SCENECHANGE_PROVIDER_TRANSITION_HARD_CUT = 1,
    SCENECHANGE_PROVIDER_TRANSITION_FADE_IN = 2,
    SCENECHANGE_PROVIDER_TRANSITION_FADE_OUT = 3,
    SCENECHANGE_PROVIDER_TRANSITION_DISSOLVE = 4,
    SCENECHANGE_PROVIDER_TRANSITION_BLACK_FRAME = 5
};

enum scenechange_provider_capability {
    SCENECHANGE_PROVIDER_CAPABILITY_OWNED_WRITE_FRAME = 1u << 0,
    SCENECHANGE_PROVIDER_CAPABILITY_RESET = 1u << 1,
    SCENECHANGE_PROVIDER_CAPABILITY_PROGRESS_CALLBACK = 1u << 2,
    SCENECHANGE_PROVIDER_CAPABILITY_TRANSITION_CLASSIFICATION = 1u << 3
};

typedef struct scenechange_provider_plane {
    uint8_t *data;
    int32_t stride;
    int32_t width;
    int32_t height;
} scenechange_provider_plane;

/*
 * The provider owns this memory until destroy. get_write_frame may return the
 * same allocation for every frame. data points at the beginning of the
 * contiguous caller-writable allocation; planes describe its image views.
 */
typedef struct scenechange_provider_frame_buffer {
    uint32_t struct_size;
    int32_t pixel_format;
    int32_t plane_count;
    uint8_t *data;
    size_t data_size;
    scenechange_provider_plane planes[4];
} scenechange_provider_frame_buffer;

#define SCENECHANGE_PROVIDER_FRAME_BUFFER_V1_SIZE \
    ((uint32_t)(offsetof(scenechange_provider_frame_buffer, planes) + sizeof(((scenechange_provider_frame_buffer *)0)->planes)))

typedef struct scenechange_provider_result {
    uint32_t struct_size;
    int64_t frame_index;
    int32_t is_scene_change;
    int32_t transition_kind;
    double confidence;
} scenechange_provider_result;

#define SCENECHANGE_PROVIDER_RESULT_V1_SIZE \
    ((uint32_t)(offsetof(scenechange_provider_result, confidence) + sizeof(((scenechange_provider_result *)0)->confidence)))

typedef int32_t (SCENECHANGE_PROVIDER_CALL *scenechange_provider_progress_callback)(
    void *user_data,
    int32_t processed_frames,
    int32_t scene_change);

/*
 * The callback is invoked synchronously after a frame result is finalized.
 * Returning non-zero requests cancellation: commit_written_frame returns
 * SCENECHANGE_PROVIDER_CANCELED, the current frame remains consumed, and the
 * result passed to commit_written_frame is valid. Callers must not retry it.
 * processed_frames starts at one, resets to zero, and saturates at INT32_MAX.
 */

typedef void *(SCENECHANGE_PROVIDER_CALL *scenechange_provider_create_fn)(
    int32_t width,
    int32_t height,
    const void *backend_options,
    size_t backend_options_size,
    char *error_buffer,
    size_t error_buffer_size);

typedef void (SCENECHANGE_PROVIDER_CALL *scenechange_provider_destroy_fn)(void *context);

typedef int32_t (SCENECHANGE_PROVIDER_CALL *scenechange_provider_reset_fn)(
    void *context,
    char *error_buffer,
    size_t error_buffer_size);

/* reset starts a new stream at frame index zero and retains the callback. */

typedef int32_t (SCENECHANGE_PROVIDER_CALL *scenechange_provider_set_progress_callback_fn)(
    void *context,
    scenechange_provider_progress_callback callback,
    void *user_data,
    char *error_buffer,
    size_t error_buffer_size);

typedef int32_t (SCENECHANGE_PROVIDER_CALL *scenechange_provider_get_write_frame_fn)(
    void *context,
    scenechange_provider_frame_buffer *frame,
    char *error_buffer,
    size_t error_buffer_size);

/*
 * get_write_frame allows one outstanding writable frame. The caller fills all
 * described plane bytes, then calls commit_written_frame exactly once. A new
 * descriptor must be requested for every frame, even when the allocation is
 * reused. A second acquisition before commit returns INVALID_ARGUMENT. The
 * returned pointer must never be freed by the caller.
 */

typedef int32_t (SCENECHANGE_PROVIDER_CALL *scenechange_provider_commit_written_frame_fn)(
    void *context,
    scenechange_provider_result *result,
    char *error_buffer,
    size_t error_buffer_size);

typedef struct scenechange_provider_api {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t backend;
    int32_t input_pixel_format;
    uint32_t capabilities;
    uint32_t reserved;
    scenechange_provider_create_fn create;
    scenechange_provider_destroy_fn destroy;
    scenechange_provider_reset_fn reset;
    scenechange_provider_set_progress_callback_fn set_progress_callback;
    scenechange_provider_get_write_frame_fn get_write_frame;
    scenechange_provider_commit_written_frame_fn commit_written_frame;
} scenechange_provider_api;

#define SCENECHANGE_PROVIDER_API_V1_SIZE \
    ((uint32_t)(offsetof(scenechange_provider_api, commit_written_frame) + sizeof(((scenechange_provider_api *)0)->commit_written_frame)))

typedef int32_t (SCENECHANGE_PROVIDER_CALL *scenechange_provider_get_api_fn)(
    uint32_t requested_version,
    scenechange_provider_api *api,
    char *error_buffer,
    size_t error_buffer_size);

SCENECHANGE_PROVIDER_EXPORT int32_t SCENECHANGE_PROVIDER_CALL scenechange_provider_get_api(
    uint32_t requested_version,
    scenechange_provider_api *api,
    char *error_buffer,
    size_t error_buffer_size);

#ifdef __cplusplus
}
#endif
