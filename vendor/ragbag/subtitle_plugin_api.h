// Copyright (c) 2026, MIRIMIRIM
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define RAGBAG_SUBTITLE_EXTERN_C extern "C"
#else
#define RAGBAG_SUBTITLE_EXTERN_C extern
#endif

#ifdef _WIN32
#ifdef RAGBAG_SUBTITLE_PLUGIN_BUILD
#define RAGBAG_SUBTITLE_PLUGIN_EXPORT RAGBAG_SUBTITLE_EXTERN_C __declspec(dllexport)
#else
#define RAGBAG_SUBTITLE_PLUGIN_EXPORT RAGBAG_SUBTITLE_EXTERN_C __declspec(dllimport)
#endif
#else
#define RAGBAG_SUBTITLE_PLUGIN_EXPORT RAGBAG_SUBTITLE_EXTERN_C __attribute__((visibility("default")))
#endif

// This ABI is deliberately limited to packet-backed bitmap subtitles used by
// Aegisub's secondary subtitle strip. It is not a general subtitle renderer API.
#define RAGBAG_SUBTITLE_DECODER_API_VERSION 1u
#define RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN INT64_MIN

typedef enum RagbagSubtitleStatusV1 {
	RAGBAG_SUBTITLE_STATUS_OK = 0,
	RAGBAG_SUBTITLE_STATUS_UNSUPPORTED_API = 1,
	RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT = 2,
	RAGBAG_SUBTITLE_STATUS_OPEN_FAILED = 3,
	RAGBAG_SUBTITLE_STATUS_DECODE_FAILED = 4,
	RAGBAG_SUBTITLE_STATUS_RENDER_FAILED = 5
} RagbagSubtitleStatusV1;

typedef enum RagbagSubtitleLogLevelV1 {
	RAGBAG_SUBTITLE_LOG_TRACE = 0,
	RAGBAG_SUBTITLE_LOG_DEBUG = 1,
	RAGBAG_SUBTITLE_LOG_INFO = 2,
	RAGBAG_SUBTITLE_LOG_WARNING = 3,
	RAGBAG_SUBTITLE_LOG_ERROR = 4
} RagbagSubtitleLogLevelV1;

typedef enum RagbagSubtitlePacketFlagV1 {
	RAGBAG_SUBTITLE_PACKET_FLAG_NONE = 0,
	RAGBAG_SUBTITLE_PACKET_FLAG_DISCONTINUITY = 1u << 0
} RagbagSubtitlePacketFlagV1;

typedef struct RagbagSubtitleHostApiV1 {
	uint32_t struct_size;
	uint32_t api_version;
	void *user_data;
	void (*log)(void *user_data, RagbagSubtitleLogLevelV1 level, const char *message_utf8);
} RagbagSubtitleHostApiV1;

typedef struct RagbagSubtitleDecoderDescriptorV1 {
	uint32_t struct_size;
	const char *decoder_id;
	const char *display_name;
	const char *debug_name;
	// Stable, host-defined codec identifiers separated by semicolons. V1
	// defines "hdmv-pgs". Container-specific codec IDs stay host-side.
	const char *codec_ids_semicolon;
} RagbagSubtitleDecoderDescriptorV1;

typedef struct RagbagSubtitleStreamInfoV1 {
	uint32_t struct_size;
	const char *codec_id;
	const uint8_t *codec_private;
	uint64_t codec_private_size;
	// Used only when the elementary stream does not declare its own authored
	// canvas. PGS normally supplies this in its presentation composition segment.
	int32_t fallback_canvas_width;
	int32_t fallback_canvas_height;
} RagbagSubtitleStreamInfoV1;

typedef struct RagbagSubtitlePacketV1 {
	uint32_t struct_size;
	int64_t pts_ns;
	int64_t dts_ns;
	int64_t duration_ns;
	uint32_t flags;
	const uint8_t *payload;
	uint64_t payload_size;
} RagbagSubtitlePacketV1;

typedef struct RagbagSubtitleRenderTargetV1 {
	uint32_t struct_size;
	// Host-owned premultiplied BGRA8 storage. plane0 points at logical row 0;
	// stride0 may be negative. render_at fully overwrites every target pixel.
	uint8_t *plane0;
	int32_t stride0;
	int32_t width;
	int32_t height;
} RagbagSubtitleRenderTargetV1;

typedef struct RagbagSubtitleRenderResultV1 {
	uint32_t struct_size;
	uint32_t has_visible_content;
	int32_t authored_width;
	int32_t authored_height;
	// The returned pixels remain valid for this half-open time interval. The
	// host may reuse its cached bitmap while subsequent requests stay inside it.
	int64_t valid_from_ns;
	int64_t valid_until_ns;
	uint64_t content_revision;
} RagbagSubtitleRenderResultV1;

typedef struct RagbagSubtitleDecoderV1 RagbagSubtitleDecoderV1;

typedef struct RagbagSubtitlePluginApiV1 {
	uint32_t struct_size;
	uint32_t api_version;
	const char *plugin_id;
	const char *plugin_name;
	const char *plugin_version;
	const char *plugin_license;

	uint32_t (*get_decoder_count)(void);
	const RagbagSubtitleDecoderDescriptorV1 *(*get_decoder_descriptor)(uint32_t index);
	int32_t (*create_decoder)(const char *decoder_id, RagbagSubtitleDecoderV1 **out_decoder);
	void (*destroy_decoder)(RagbagSubtitleDecoderV1 *decoder);
	// decoder may be null immediately after create_decoder fails; implementations
	// may then return a thread-local plugin error from the failed ABI call.
	const char *(*get_last_error)(RagbagSubtitleDecoderV1 *decoder);
	int32_t (*begin_stream)(RagbagSubtitleDecoderV1 *decoder, const RagbagSubtitleStreamInfoV1 *stream);
	int32_t (*push_packet)(RagbagSubtitleDecoderV1 *decoder, const RagbagSubtitlePacketV1 *packet);
	int32_t (*end_stream)(RagbagSubtitleDecoderV1 *decoder);
	int32_t (*render_at)(RagbagSubtitleDecoderV1 *decoder, int64_t time_ns, RagbagSubtitleRenderTargetV1 *target, RagbagSubtitleRenderResultV1 *result);
} RagbagSubtitlePluginApiV1;

RAGBAG_SUBTITLE_PLUGIN_EXPORT int32_t ragbag_subtitle_decoder_init_v1(
	const RagbagSubtitleHostApiV1 *host,
	RagbagSubtitlePluginApiV1 *plugin);
