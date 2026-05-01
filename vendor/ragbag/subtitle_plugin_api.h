// Copyright (c) 2026, MIRIMIRIM

#pragma once

#include <stdint.h>

#ifdef _WIN32
#ifdef RAGBAG_SUBTITLE_PLUGIN_BUILD
#define RAGBAG_SUBTITLE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define RAGBAG_SUBTITLE_PLUGIN_EXPORT extern "C" __declspec(dllimport)
#endif
#else
#define RAGBAG_SUBTITLE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define RAGBAG_SUBTITLE_PLUGIN_API_VERSION 0u

typedef enum RagbagSubtitleStatusV0 {
	RAGBAG_SUBTITLE_STATUS_OK = 0,
	RAGBAG_SUBTITLE_STATUS_UNSUPPORTED_API = 1,
	RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT = 2,
	RAGBAG_SUBTITLE_STATUS_OPEN_FAILED = 3,
	RAGBAG_SUBTITLE_STATUS_DECODE_FAILED = 4,
	RAGBAG_SUBTITLE_STATUS_RENDER_FAILED = 5
} RagbagSubtitleStatusV0;

typedef enum RagbagSubtitleLogLevelV0 {
	RAGBAG_SUBTITLE_LOG_TRACE = 0,
	RAGBAG_SUBTITLE_LOG_DEBUG = 1,
	RAGBAG_SUBTITLE_LOG_INFO = 2,
	RAGBAG_SUBTITLE_LOG_WARNING = 3,
	RAGBAG_SUBTITLE_LOG_ERROR = 4
} RagbagSubtitleLogLevelV0;

typedef enum RagbagSubtitlePixelFormatV0 {
	RAGBAG_SUBTITLE_PIXEL_FORMAT_UNKNOWN = 0,
	RAGBAG_SUBTITLE_PIXEL_FORMAT_BGRA8 = 1
} RagbagSubtitlePixelFormatV0;

typedef enum RagbagSubtitleAlphaModeV0 {
	RAGBAG_SUBTITLE_ALPHA_UNKNOWN = 0,
	RAGBAG_SUBTITLE_ALPHA_PREMULTIPLIED = 1
} RagbagSubtitleAlphaModeV0;

typedef enum RagbagSubtitleProviderCapabilityV0 {
	RAGBAG_SUBTITLE_PROVIDER_CAP_FILE_INPUT = 1u << 0,
	RAGBAG_SUBTITLE_PROVIDER_CAP_BITMAP_OUTPUT = 1u << 1,
	RAGBAG_SUBTITLE_PROVIDER_CAP_TEXT_OUTPUT = 1u << 2,
	RAGBAG_SUBTITLE_PROVIDER_CAP_DIRTY_RECTS = 1u << 3,
	RAGBAG_SUBTITLE_PROVIDER_CAP_PREMULTIPLIED_BGRA8 = 1u << 4
} RagbagSubtitleProviderCapabilityV0;

typedef enum RagbagSubtitleRenderFlagV0 {
	RAGBAG_SUBTITLE_RENDER_CLEAR_TARGET = 1u << 0
} RagbagSubtitleRenderFlagV0;

typedef struct RagbagSubtitleHostApiV0 {
	uint32_t struct_size;
	uint32_t api_version;
	void *user_data;
	void (*log)(void *user_data, RagbagSubtitleLogLevelV0 level, const char *message_utf8);
} RagbagSubtitleHostApiV0;

typedef struct RagbagSubtitleProviderDescriptorV0 {
	uint32_t struct_size;
	const char *provider_id;
	const char *display_name;
	const char *debug_name;
	const char *extensions_semicolon;
	const char *codec_names_semicolon;
	uint32_t capabilities;
} RagbagSubtitleProviderDescriptorV0;

typedef struct RagbagSubtitleVideoInfoV0 {
	uint32_t struct_size;
	int32_t width;
	int32_t height;
	int32_t storage_width;
	int32_t storage_height;
	int32_t color_primaries;
	int32_t color_transfer;
	int32_t color_matrix;
	int32_t color_range;
	double fps;
} RagbagSubtitleVideoInfoV0;

typedef struct RagbagSubtitleRenderRequestV0 {
	uint32_t struct_size;
	double time_seconds;
	RagbagSubtitleVideoInfoV0 video;
	uint32_t flags;
} RagbagSubtitleRenderRequestV0;

typedef struct RagbagSubtitleDirtyRectV0 {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
} RagbagSubtitleDirtyRectV0;

typedef struct RagbagSubtitleOverlayTargetV0 {
	uint32_t struct_size;
	RagbagSubtitlePixelFormatV0 pixel_format;
	RagbagSubtitleAlphaModeV0 alpha_mode;
	uint8_t *plane0;
	int32_t stride0;      // bytes per row; negative = bottom-up (flipped)
	int32_t width;
	int32_t height;
	RagbagSubtitleDirtyRectV0 *dirty_rects;
	uint32_t dirty_rect_capacity;
	uint32_t dirty_rect_count;
	uint32_t has_visible_content;
} RagbagSubtitleOverlayTargetV0;

typedef struct RagbagSubtitleProviderV0 RagbagSubtitleProviderV0;

typedef struct RagbagSubtitlePluginApiV0 {
	uint32_t struct_size;
	uint32_t api_version;
	const char *plugin_id;
	const char *plugin_name;
	const char *plugin_version;
	const char *plugin_license;

	uint32_t (*get_provider_count)(void);
	const RagbagSubtitleProviderDescriptorV0 *(*get_provider_descriptor)(uint32_t index);
	int32_t (*create_provider)(const char *provider_id, RagbagSubtitleProviderV0 **out_provider);
	void (*destroy_provider)(RagbagSubtitleProviderV0 *provider);
	const char *(*get_last_error)(RagbagSubtitleProviderV0 *provider);
	int32_t (*open_file)(RagbagSubtitleProviderV0 *provider, const char *path_utf8, const RagbagSubtitleVideoInfoV0 *video_hint);
	int32_t (*render_overlay)(RagbagSubtitleProviderV0 *provider, const RagbagSubtitleRenderRequestV0 *request, RagbagSubtitleOverlayTargetV0 *target);
} RagbagSubtitlePluginApiV0;

RAGBAG_SUBTITLE_PLUGIN_EXPORT int32_t ragbag_subtitle_plugin_init_v0(
	const RagbagSubtitleHostApiV0 *host,
	RagbagSubtitlePluginApiV0 *plugin);
