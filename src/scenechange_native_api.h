#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_MSC_VER)
#define SCENECHANGE_NATIVE_CALL __cdecl
#else
#define SCENECHANGE_NATIVE_CALL
#endif

extern "C" {

struct scenechange_wwxd_context_t;

using scenechange_get_api_version_fn = int32_t (SCENECHANGE_NATIVE_CALL *)();
using scenechange_get_versions_json_utf8_fn = char* (SCENECHANGE_NATIVE_CALL *)(char **error_message);
using scenechange_free_fn = void (SCENECHANGE_NATIVE_CALL *)(void *p);
using scenechange_progress_callback_fn = int32_t (SCENECHANGE_NATIVE_CALL *)(
	void *user_data,
	int32_t processed_frames,
	int32_t scene_change);
using scenechange_wwxd_create_fn = scenechange_wwxd_context_t* (SCENECHANGE_NATIVE_CALL *)(
	int32_t width,
	int32_t height,
	char *error_buffer,
	std::size_t error_buffer_size);
using scenechange_wwxd_destroy_fn = void (SCENECHANGE_NATIVE_CALL *)(scenechange_wwxd_context_t *context);
using scenechange_wwxd_set_progress_callback_fn = int32_t (SCENECHANGE_NATIVE_CALL *)(
	scenechange_wwxd_context_t *context,
	scenechange_progress_callback_fn callback,
	void *user_data,
	char *error_buffer,
	std::size_t error_buffer_size);
using scenechange_wwxd_get_write_buffer_fn = std::uint8_t* (SCENECHANGE_NATIVE_CALL *)(
	scenechange_wwxd_context_t *context,
	int32_t *stride,
	char *error_buffer,
	std::size_t error_buffer_size);
using scenechange_wwxd_commit_written_frame_no_pad_fn = int32_t (SCENECHANGE_NATIVE_CALL *)(
	scenechange_wwxd_context_t *context,
	int32_t *scene_change,
	char *error_buffer,
	std::size_t error_buffer_size);
}

namespace scenechange {

constexpr int32_t kApiVersion = 0x00010000;

struct Api {
	scenechange_get_api_version_fn get_api_version = nullptr;
	scenechange_get_versions_json_utf8_fn get_versions_json_utf8 = nullptr;
	scenechange_free_fn free = nullptr;
	scenechange_wwxd_create_fn wwxd_create = nullptr;
	scenechange_wwxd_destroy_fn wwxd_destroy = nullptr;
	scenechange_wwxd_set_progress_callback_fn wwxd_set_progress_callback = nullptr;
	scenechange_wwxd_get_write_buffer_fn wwxd_get_write_buffer = nullptr;
	scenechange_wwxd_commit_written_frame_no_pad_fn wwxd_commit_written_frame_no_pad = nullptr;
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
Api const& GetApi();

} // namespace scenechange
