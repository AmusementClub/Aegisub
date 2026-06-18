#pragma once

#include <stddef.h>
#include <stdint.h>

/* Public C ABI for the GUI-independent Aegisub core.
 *
 * This header is the external contract for generated bindings (for example
 * ClangSharp), hand-written P/Invoke, Rust FFI, and native consumers. It
 * intentionally exposes only plain C data: opaque handles, fixed-width integer
 * types, pointer/length strings, and caller-allocated output buffers. Do not
 * depend on implementation-language object types, exceptions, allocators,
 * standard-library layouts, GUI framework types, or process-global GUI state
 * across this boundary.
 *
 * Binding-generation notes:
 *   - Map size_t to the target language's pointer-sized unsigned type
 *     (for example nuint/UIntPtr in .NET), not to a fixed 64-bit integer.
 *   - Map uint8_t boolean fields as one-byte integer/boolean values. Inputs
 *     treat any non-zero value as true; outputs are normalized to 0 or 1.
 *   - Treat aegisub_core_string as a borrowed pointer/length view, not as a
 *     default-marshalled C string. It may contain embedded NUL bytes and is
 *     not required to be NUL terminated.
 *
 * The preferred cross-language usage pattern is:
 *   1. Check aegisub_core_abi_version() against AEGISUB_CORE_ABI_VERSION.
 *   2. Create one aegisub_core_context for host hooks, diagnostics, and
 *      last-error storage.
 *   3. Register provider factories needed by this process. Most non-wx hosts
 *      should call aegisub_core_register_builtin_provider_factories() first,
 *      then add any host/plugin factories exposed by future APIs.
 *   4. Finalize the process-global provider registry before opening
 *      provider-backed media sessions.
 *   5. Open media/subtitle sessions as opaque handles.
 *   6. Move large data through windowed batch APIs instead of per-row calls.
 *   7. Copy borrowed string views before destroying a handle or mutating the
 *      subtitle session that produced them. */

/* Public calling convention for exported C ABI functions. Bindings must use
 * this convention for callbacks too; on Windows it is cdecl. In generated
 * bindings this belongs on both imported functions and callback delegates. */
#ifndef AEGISUB_CORE_CALL
#if defined(_WIN32)
#define AEGISUB_CORE_CALL __cdecl
#else
#define AEGISUB_CORE_CALL
#endif
#endif

/* Symbol visibility/import macro.
 * - Define AEGISUB_CORE_BUILD_SHARED when building the shared library.
 * - Define AEGISUB_CORE_USE_SHARED when consuming the shared library on
 *   Windows.
 * - Leave both undefined when consuming the static library.
 *
 * This macro only controls symbol import/export annotations. Static consumers
 * are still responsible for linking the static C API library and its native
 * dependencies according to the package/build-system metadata for the build
 * they consume. Bindings which load the shared library dynamically at runtime
 * usually leave this undefined. */
#ifndef AEGISUB_CORE_API
#if defined(_WIN32) && defined(AEGISUB_CORE_BUILD_SHARED)
#define AEGISUB_CORE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(AEGISUB_CORE_USE_SHARED)
#define AEGISUB_CORE_API __declspec(dllimport)
#elif !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define AEGISUB_CORE_API __attribute__((visibility("default")))
#else
#define AEGISUB_CORE_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* C ABI conventions:
 *
 * - All text is UTF-8 byte text. aegisub_core_string values are length-based
 *   and are not required to be NUL terminated.
 * - Callers may zero-initialize every public struct, then set abi_version where
 *   present and struct_size where present. Functions reject structs whose
 *   abi_version differs from AEGISUB_CORE_ABI_VERSION. struct_size is accepted
 *   as zero for legacy zero-initialized callers; see "Struct evolution" below.
 * - Integer row indexes and counts use zero-based indexing. size_t counts are
 *   buffer sizes or row/provider/attempt counts, never byte sizes unless the
 *   field name says so.
 * - Output handles are set to NULL before work starts. Destroy functions and
 *   aegisub_core_free_string() accept NULL storage.
 * - Output count pointers named written/applied/deleted/etc. are set to zero
 *   before validation when the pointer itself is non-NULL. On success they
 *   describe what was copied or changed by that call; on failure they remain
 *   zero.
 * - Non-count output structs/scalars are written only on OK unless a function
 *   explicitly documents different behavior. Callers should not inspect them
 *   after failure.
 * - Borrowed strings and arrays stay valid only for the lifetime documented on
 *   the handle that produced them. Copy data before destroying that handle or
 *   before calling a mutating function on the same session.
 * - NULL input arrays are accepted only when the matching count is zero unless
 *   a function explicitly documents a different rule.
 * - APIs are coarse-grained where data can be large. Prefer batch functions
 *   over per-item calls across managed/native or process boundaries.
 * - Handles are not internally synchronized. Hosts should serialize concurrent
 *   access to the same context/session/plan/catalog/report handle.
 * - Functions returning aegisub_core_status never throw implementation
 *   exceptions across the ABI boundary. On failure, a context-taking function
 *   may update the context last-error string with a diagnostic for
 *   display/logging. */

/* Public ABI version expected by every options/input structure that has an
 * abi_version field. Increment this ONLY for a breaking layout change that
 * cannot be expressed as a tail-field addition (see "Struct evolution" below).
 * Adding a field to the end of an existing struct does NOT require bumping this
 * version; bumping it rejects all structs compiled against any other version.
 * Call aegisub_core_abi_version() at runtime to reject a DLL/header mismatch
 * before using any other API. */
enum {
	AEGISUB_CORE_ABI_VERSION = 16
};

/* Struct evolution (tail-field extension):
 *
 * Input/options structs that carry abi_version also carry a struct_size field
 * immediately after abi_version. Callers set struct_size to sizeof(the struct
 * as compiled in their header). Core interprets it as follows:
 *
 *   - struct_size == 0: legacy zero-initialized caller. Core treats the struct
 *     as the size it was compiled against and reads only known fields. Existing
 *     callers that zero-initialize and set only abi_version keep working.
 *   - struct_size < offsetof(known_last_field) + sizeof(known_last_field):
 *     caller was compiled against an older header than core. Rejected with
 *     AEGISUB_CORE_STATUS_INVALID_ARGUMENT, since core cannot trust fields it
 *     needs were present.
 *   - struct_size == sizeof(struct as compiled in core): exact match.
 *   - struct_size > sizeof(struct as compiled in core): caller was compiled
 *     against a newer header. Core reads only the fields it knows about and
 *     ignores the trailing bytes. This is the forward-compatible path: new
 *     fields can be appended to a struct without bumping AEGISUB_CORE_ABI_VERSION.
 *
 * Appending a field therefore only requires: (1) adding it to the end of the
 * struct, (2) updating core to read it, (3) updating the known-minimum size
 * check in the matching Convert function once the field becomes required.
 *
 * Reordering, removing, or repurposing an existing field, or inserting a field
 * anywhere other than the tail, IS a breaking change: bump
 * AEGISUB_CORE_ABI_VERSION and reject all older structs. */


/* Opaque native handles. Consumers must not allocate, embed, copy by value, or
 * free these directly. Store only the pointer value returned by the API and
 * release it with the matching destroy function. Managed bindings should wrap
 * owning handles in SafeHandle-like types so exceptions cannot leak native
 * state. Borrowed strings/arrays returned through these handles are invalidated
 * exactly as documented on the producing API. */
typedef struct aegisub_core_context aegisub_core_context;
typedef struct aegisub_core_provider_catalog aegisub_core_provider_catalog;
typedef struct aegisub_core_provider_open_report aegisub_core_provider_open_report;
typedef struct aegisub_core_video_session aegisub_core_video_session;
typedef struct aegisub_core_audio_session aegisub_core_audio_session;
typedef struct aegisub_core_subtitle_session aegisub_core_subtitle_session;
typedef struct aegisub_core_grid_selection_plan aegisub_core_grid_selection_plan;

/* Non-owning UTF-8 byte string. data may be NULL only when size is zero.
 * data is not required to end with '\0'. Inputs are copied during the call
 * unless a function explicitly says otherwise. Outputs are usually borrowed
 * from the handle that produced them. */
typedef struct aegisub_core_string {
	const char *data;
	size_t size;
} aegisub_core_string;

/* Owned UTF-8 byte string allocated by the core API. Release with
 * aegisub_core_free_string(); never free it with the caller's allocator. */
typedef struct aegisub_core_owned_string {
	char *data;
	size_t size;
} aegisub_core_owned_string;

/* Stable status codes for the C ABI. OK is the only success value. Other
 * values are grouped enough for host decisions; use last_error for text that is
 * useful to users or logs. */
typedef int32_t aegisub_core_status;
enum {
	AEGISUB_CORE_STATUS_OK = 0,
	AEGISUB_CORE_STATUS_CANCELLED = 1,
	AEGISUB_CORE_STATUS_INVALID_ARGUMENT = 2,
	AEGISUB_CORE_STATUS_FILE_NOT_FOUND = 3,
	AEGISUB_CORE_STATUS_FILE_SYSTEM_ERROR = 4,
	AEGISUB_CORE_STATUS_NOT_SUPPORTED = 5,
	AEGISUB_CORE_STATUS_NO_MEDIA = 6,
	AEGISUB_CORE_STATUS_REGISTRY_FINALIZED = 7,
	AEGISUB_CORE_STATUS_TIMED_OUT = 8,
	AEGISUB_CORE_STATUS_ERROR = 100
};

/* Provider family for catalog and open-report queries. */
typedef int32_t aegisub_core_provider_kind;
enum {
	AEGISUB_CORE_PROVIDER_AUDIO = 0,
	AEGISUB_CORE_PROVIDER_VIDEO = 1,
	AEGISUB_CORE_PROVIDER_SUBTITLES = 2
};

/* Sort columns supported by Aegisub subtitle row order operations. */
typedef int32_t aegisub_core_subtitle_sort_key;
enum {
	AEGISUB_CORE_SUBTITLE_SORT_START = 0,
	AEGISUB_CORE_SUBTITLE_SORT_END = 1,
	AEGISUB_CORE_SUBTITLE_SORT_STYLE = 2,
	AEGISUB_CORE_SUBTITLE_SORT_ACTOR = 3,
	AEGISUB_CORE_SUBTITLE_SORT_EFFECT = 4,
	AEGISUB_CORE_SUBTITLE_SORT_LAYER = 5
};

/* Field mask for aegisub_core_subtitle_row_patch. Fields whose bits are not
 * set are ignored, even if their struct members contain non-zero data. */
typedef uint32_t aegisub_core_subtitle_row_patch_fields;
enum {
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_COMMENT = 1 << 0,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_LAYER = 1 << 1,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_START = 1 << 2,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_END = 1 << 3,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_MARGINS = 1 << 4,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_STYLE = 1 << 5,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_ACTOR = 1 << 6,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_EFFECT = 1 << 7,
	AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT = 1 << 8
};

/* Coarse row-change categories reported in aegisub_core_subtitle_state.
 * These are intentionally broader than row patch fields: they are meant for
 * GUI invalidation decisions across C/.NET/Rust/etc., not for reconstructing
 * the edit. Combine with bitwise OR and test with bitwise AND. */
typedef uint32_t aegisub_core_subtitle_change_fields;
enum {
	AEGISUB_CORE_SUBTITLE_CHANGE_NONE = 0,
	/* Dialogue text changed. Text layout/rendering caches may be stale. */
	AEGISUB_CORE_SUBTITLE_CHANGE_TEXT = 1 << 0,
	/* Start or end time changed. Timeline/video/subtitle visibility caches may
	 * be stale. */
	AEGISUB_CORE_SUBTITLE_CHANGE_TIME = 1 << 1,
	/* Non-text row metadata changed, such as comment, layer, margins, style,
	 * actor, or effect. */
	AEGISUB_CORE_SUBTITLE_CHANGE_METADATA = 1 << 2,
	/* The row set changed: rows were inserted or removed and row indexes at or
	 * after row_change_first_row may have shifted. */
	AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE = 1 << 3
};

/* Field mask for paste-over source rows. These mirror the existing Paste Lines
 * Over UI fields, including separate margin bits. */
typedef uint32_t aegisub_core_subtitle_paste_over_fields;
enum {
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_COMMENT = 1 << 0,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_LAYER = 1 << 1,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_START = 1 << 2,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_END = 1 << 3,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_STYLE = 1 << 4,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_ACTOR = 1 << 5,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_MARGIN_LEFT = 1 << 6,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_MARGIN_RIGHT = 1 << 7,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_MARGIN_VERTICAL = 1 << 8,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_EFFECT = 1 << 9,
	AEGISUB_CORE_SUBTITLE_PASTE_OVER_TEXT = 1 << 10
};

/* Provider descriptors are borrowed from the provider catalog handle. */
typedef struct aegisub_core_provider_descriptor {
	/* Provider identifier used for preferred_provider inputs. */
	aegisub_core_string name;
	/* User-facing provider name. May match name when there is no separate
	 * display label. */
	aegisub_core_string display_name;
	/* User-facing reason the provider cannot currently be used. Empty when
	 * available is non-zero. */
	aegisub_core_string unavailable_reason;
	/* Non-zero when normal UI should hide the provider unless explicitly asked
	 * to show advanced/internal choices. */
	uint8_t hidden;
	/* Non-zero when the provider can currently be opened. */
	uint8_t available;
} aegisub_core_provider_descriptor;

/* Provider open reports are snapshots owned by the report handle. They remain
 * stable until aegisub_core_provider_open_report_destroy(). */
typedef struct aegisub_core_provider_open_report_info {
	/* Provider family this report describes. */
	aegisub_core_provider_kind kind;
	/* Requested provider from the open options, or empty when default provider
	 * order was used. */
	aegisub_core_string preferred_provider;
	/* Provider selected by the core, or empty when no provider opened. */
	aegisub_core_string selected_provider;
	/* Number of provider attempts available through *_attempts_get. */
	size_t attempt_count;
} aegisub_core_provider_open_report_info;

typedef struct aegisub_core_provider_open_attempt {
	/* Provider identifier that was attempted. */
	aegisub_core_string provider_name;
	/* Short user/log-facing result such as success, unavailable, or failed. */
	aegisub_core_string outcome;
	/* Optional diagnostic detail for provider setup/open failures. */
	aegisub_core_string detail;
} aegisub_core_provider_open_attempt;

/* Options for opening a video provider-backed session. Zero-initialize, then
 * set abi_version to AEGISUB_CORE_ABI_VERSION, set struct_size to sizeof(this
 * struct), and fill desired fields. struct_size enables tail-field evolution:
 * callers built against a newer header may pass a larger struct and core reads
 * only the fields it knows about. See "Struct evolution" in the header notes. */
typedef struct aegisub_core_video_open_options {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_video_open_options). Zero is treated as the legacy
	 * zero-initialized caller size. */
	size_t struct_size;
	/* UTF-8 filesystem path or provider-specific pseudo path. */
	aegisub_core_string path;
	/* Optional color matrix hint; pass empty to use provider/default behavior. */
	aegisub_core_string colormatrix;
	/* Optional provider name; pass empty to use the configured/default order. */
	aegisub_core_string preferred_provider;
	/* Optional provider cache limit. Zero means provider/default behavior. */
	uint64_t max_cache_size_bytes;
} aegisub_core_video_open_options;

/* Video metadata snapshot. Numeric fields are copied values. String fields are
 * borrowed from the video session and become invalid when it is destroyed. */
typedef struct aegisub_core_video_info {
	/* Total frame count reported by the provider. */
	int32_t frame_count;
	/* Encoded/display frame dimensions in pixels. */
	int32_t width;
	int32_t height;
	/* Display aspect ratio. */
	double dar;
	/* Nominal frames per second. For VFR sources this is a summary value; use
	 * future time-map APIs for exact frame/time conversion. */
	double fps;
	/* Non-zero when the source has variable frame timing. */
	uint8_t is_vfr;
	/* Non-zero when the container/provider reports embedded audio. */
	uint8_t has_audio;
	/* Non-zero when opening this video should update subtitle script video
	 * properties such as resolution. */
	uint8_t should_set_video_properties;
	/* Non-zero when the provider recommends a cache for responsive playback. */
	uint8_t wants_caching;
	/* Borrowed from the video session. */
	aegisub_core_string selected_provider;
	aegisub_core_string decoder_name;
	aegisub_core_string color_space;
	aegisub_core_string real_color_space;
	aegisub_core_string warning;
	aegisub_core_string native_format_description;
	size_t keyframe_count;
} aegisub_core_video_info;

/* CPU BGRA frame snapshot metadata. Frame data returned by
 * aegisub_core_video_frame_bgra_get is tightly described by these fields but is
 * still owned by the caller-provided buffer. This path is intended for
 * non-renderer consumers such as thumbnails, probes, and simple UI previews;
 * renderer-owned GL/native frame interop is a separate boundary. */
typedef struct aegisub_core_video_frame_info {
	int32_t width;
	int32_t height;
	size_t pitch;
	size_t data_size;
	uint8_t flipped;
} aegisub_core_video_frame_info;

/* Options for opening an audio provider-backed session. Zero-initialize, then
 * set abi_version to AEGISUB_CORE_ABI_VERSION, set struct_size to sizeof(this
 * struct), and fill desired fields. See "Struct evolution" in the header notes. */
typedef struct aegisub_core_audio_open_options {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_audio_open_options). Zero is treated as the legacy
	 * zero-initialized caller size. */
	size_t struct_size;
	/* UTF-8 filesystem path or provider-specific pseudo path. */
	aegisub_core_string path;
	/* Optional provider name; pass empty to use the configured/default order. */
	aegisub_core_string preferred_provider;
} aegisub_core_audio_open_options;

/* Audio metadata snapshot. Numeric fields are copied values. String fields are
 * borrowed from the audio session and become invalid when it is destroyed. */
typedef struct aegisub_core_audio_info {
	/* Total sample count in the logical audio stream. */
	int64_t num_samples;
	/* Number of samples currently decoded/available through the provider. */
	int64_t decoded_samples;
	/* Samples per second. */
	int32_t sample_rate;
	/* Bytes per sample per channel. */
	int32_t bytes_per_sample;
	/* Channel count. */
	int32_t channels;
	/* Non-zero for floating-point sample data. */
	uint8_t float_samples;
	/* Non-zero when the source should be cached before interactive use. */
	uint8_t source_needs_cache;
	/* Approximate logical stream size in bytes. */
	uint64_t logical_bytes;
	/* Approximate decoded data size in bytes. */
	uint64_t decoded_bytes;
	/* Borrowed from the audio session. */
	aegisub_core_string selected_provider;
	aegisub_core_string storage_kind;
} aegisub_core_audio_info;

/* Options for opening a subtitle document. Zero-initialize, then set
 * abi_version to AEGISUB_CORE_ABI_VERSION, set struct_size to sizeof(this
 * struct), and fill desired fields. See "Struct evolution" in the header notes. */
typedef struct aegisub_core_subtitle_open_options {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_subtitle_open_options). Zero is treated as the legacy
	 * zero-initialized caller size. */
	size_t struct_size;
	/* UTF-8 filesystem path. */
	aegisub_core_string path;
	/* Optional text encoding name. Pass empty for format/default detection. */
	aegisub_core_string encoding;
} aegisub_core_subtitle_open_options;

/* Options for saving a subtitle document. Zero-initialize, then set
 * abi_version to AEGISUB_CORE_ABI_VERSION, set struct_size to sizeof(this
 * struct), and fill desired fields. See "Struct evolution" in the header notes. */
typedef struct aegisub_core_subtitle_save_options {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_subtitle_save_options). Zero is treated as the legacy
	 * zero-initialized caller size. */
	size_t struct_size;
	/* UTF-8 filesystem path. Existing files may be overwritten. */
	aegisub_core_string path;
	/* Optional output encoding name. Pass empty for utf-8. */
	aegisub_core_string encoding;
} aegisub_core_subtitle_save_options;

/* Subtitle metadata snapshot at the time of the info_get call. row_count is
 * useful when the host already needs the rest of this snapshot, but hosts that
 * only need the current row count should prefer aegisub_core_subtitle_row_count()
 * as the narrower call. Do not cache row_count across row insert/delete APIs. */
typedef struct aegisub_core_subtitle_info {
	/* Number of dialogue/comment rows available through row APIs. */
	int32_t row_count;
	/* Number of style records in the document. */
	int32_t style_count;
	/* Number of attached fonts/graphics in the document. */
	int32_t attachment_count;
	/* Number of script info key/value entries. */
	int32_t script_info_count;
	/* Script resolution in pixels. */
	int32_t width;
	int32_t height;
	/* Core-owned enum value describing how resolution was inferred. Hosts
	 * should treat unknown values as informational. */
	int32_t resolution_type;
	/* Borrowed from the subtitle session. */
	aegisub_core_string format_name;
} aegisub_core_subtitle_info;

/* Mutable subtitle session state for GUI refresh/save decisions.
 * revision starts at 0 and increments once per successful mutating subtitle
 * call that changes row data. Failed or empty edits do not change it. dirty is
 * set by successful edits and cleared by successful save; saving does not reset
 * revision.
 *
 * row_change_* describes the most recent successful row-data edit as a
 * contiguous invalidation window [first_row, first_row + row_count). Sparse
 * edits are intentionally widened to one range so GUI hosts can refresh a
 * virtualized row cache with one cheap state read. Row insert/delete edits set
 * AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE; for tail deletion the invalidation
 * window may be empty, so row_change_revision and row_change_fields are the
 * authoritative signals that a structural edit happened. row_change_revision is
 * zero and row_change_fields is AEGISUB_CORE_SUBTITLE_CHANGE_NONE until the
 * first row edit. Saves do not change the row-change window or fields.
 *
 * row_change_fields contains aegisub_core_subtitle_change_fields bits for that
 * same most-recent row edit. It is AEGISUB_CORE_SUBTITLE_CHANGE_NONE until the
 * first row edit. Hosts should treat these as invalidation hints only; use
 * aegisub_core_subtitle_rows_get() to read current row data. */
typedef struct aegisub_core_subtitle_state {
	/* Monotonic edit revision for row-data changes. */
	uint64_t revision;
	/* revision value of the most recent row change, or zero before any row
	 * change. */
	uint64_t row_change_revision;
	/* First row in the most recent invalidation window. */
	size_t row_change_first_row;
	/* Number of rows in the most recent invalidation window. May be zero for
	 * no-op edits and tail deletions. */
	size_t row_change_row_count;
	/* Coarse invalidation bits for the most recent row change. */
	aegisub_core_subtitle_change_fields row_change_fields;
	/* Non-zero when the subtitle document has unsaved changes. */
	uint8_t dirty;
} aegisub_core_subtitle_state;

/* Row snapshot for display/editing. String views are borrowed from the
 * subtitle session and become invalid after destroying the session or after any
 * successful mutating subtitle API call on the same session. */
typedef struct aegisub_core_subtitle_row {
	/* Stable Aegisub line id for the row while it remains in the document. */
	int32_t line_id;
	/* Current zero-based display index. This may change after insert/delete,
	 * move, duplicate, or sort operations. */
	int32_t row_index;
	/* Non-zero for ASS/SSA Comment lines. */
	uint8_t comment;
	int32_t layer;
	/* Start/end times in milliseconds. */
	int32_t start_ms;
	int32_t end_ms;
	/* ASS margins in script pixels. */
	int32_t margin_left;
	int32_t margin_right;
	int32_t margin_vertical;
	/* Borrowed UTF-8 row strings. */
	aegisub_core_string style;
	aegisub_core_string actor;
	aegisub_core_string effect;
	aegisub_core_string text;
} aegisub_core_subtitle_row;

/* Text-only edit kept for compatibility and simple integrations. New editor
 * code should prefer aegisub_core_subtitle_row_patches_apply(). */
typedef struct aegisub_core_subtitle_row_text_edit {
	/* Zero-based row index to edit. */
	size_t row;
	/* Replacement dialogue text. */
	aegisub_core_string text;
} aegisub_core_subtitle_row_text_edit;

/* Field-mask based row edit. Only members selected by fields are read; string
 * fields whose mask bit is clear may be left as {NULL, 0}. A patch with
 * fields == 0 validates its row index but applies no row data and does not
 * advance revision/dirty state by itself. Patch application is all-or-none:
 * invalid row indexes, unknown field bits, negative times/margins, or start >
 * end reject the whole batch and apply no rows. */
typedef struct aegisub_core_subtitle_row_patch {
	/* Zero-based row index to edit. */
	size_t row;
	/* Selected fields to apply. Unknown bits are invalid. */
	aegisub_core_subtitle_row_patch_fields fields;
	uint8_t comment;
	int32_t layer;
	/* Selected start/end times in milliseconds. Times must be non-negative and
	 * the resulting row range must satisfy start <= end. */
	int32_t start_ms;
	int32_t end_ms;
	/* Selected ASS margins in script pixels. Margins must be non-negative. */
	int32_t margin_left;
	int32_t margin_right;
	int32_t margin_vertical;
	/* Selected UTF-8 replacement strings. */
	aegisub_core_string style;
	aegisub_core_string actor;
	aegisub_core_string effect;
	aegisub_core_string text;
} aegisub_core_subtitle_row_patch;

/* Source row data for paste-over. Only fields selected by
 * aegisub_core_subtitle_paste_over_fields are read. Unselected string fields
 * may be left as {NULL, 0}; selected string fields must follow
 * aegisub_core_string's input contract. */
typedef struct aegisub_core_subtitle_paste_over_source {
	uint8_t comment;
	int32_t layer;
	/* Source start/end times in milliseconds. Used only when selected by the
	 * paste-over field mask. */
	int32_t start_ms;
	int32_t end_ms;
	/* Source ASS margins in script pixels. Used only when selected by the
	 * paste-over field mask. */
	int32_t margin_left;
	int32_t margin_right;
	int32_t margin_vertical;
	/* Source UTF-8 strings. Used only when selected by the paste-over field
	 * mask. */
	aegisub_core_string style;
	aegisub_core_string actor;
	aegisub_core_string effect;
	aegisub_core_string text;
} aegisub_core_subtitle_paste_over_source;

/* Keyboard/mouse modifiers using GUI-neutral names. Values are booleans:
 * zero is false, non-zero is true. */
typedef struct aegisub_core_grid_modifier_state {
	uint8_t shift;
	uint8_t ctrl;
	uint8_t alt;
} aegisub_core_grid_modifier_state;

/* Stateless mouse-selection input for subtitle grid policy.
 * selected_rows may be NULL only when selected_row_count is zero. row indexes
 * are zero-based. target_row/anchor_row may be -1 when there is no row. Boolean
 * fields are independent event-state flags from the host; core does not query
 * GUI framework state. */
typedef struct aegisub_core_grid_mouse_selection_input {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_grid_mouse_selection_input). Zero is treated as the
	 * legacy zero-initialized caller size. */
	size_t struct_size;
	/* Current number of subtitle rows in the host grid/model. */
	int32_t row_count;
	/* Row under the mouse event, or -1 for no row. */
	int32_t target_row;
	/* Current range-selection anchor row, or -1 when the host has none. */
	int32_t anchor_row;
	/* Current selected rows in display order. Borrowed only for this call. */
	int32_t const *selected_rows;
	size_t selected_row_count;
	/* Non-zero when this input represents a normal click activation. */
	uint8_t click;
	/* Non-zero when this input represents a double-click activation. */
	uint8_t double_click;
	/* Non-zero while extending/updating a drag selection. */
	uint8_t dragging;
	aegisub_core_grid_modifier_state modifiers;
} aegisub_core_grid_mouse_selection_input;

/* Stateless keyboard-selection input for subtitle grid policy.
 * direction is normally -1 or +1, and step is the row/page distance requested
 * by the host. active_row/anchor_row may be -1 when there is no row. */
typedef struct aegisub_core_grid_keyboard_selection_input {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_grid_keyboard_selection_input). Zero is treated as
	 * the legacy zero-initialized caller size. */
	size_t struct_size;
	/* Current number of subtitle rows in the host grid/model. */
	int32_t row_count;
	/* Current focused/active row, or -1 when the host has none. */
	int32_t active_row;
	/* Current range-selection anchor row, or -1 when the host has none. */
	int32_t anchor_row;
	/* Current selected rows in display order. Borrowed only for this call. */
	int32_t const *selected_rows;
	size_t selected_row_count;
	/* Requested movement direction, normally -1 for up or +1 for down. */
	int32_t direction;
	/* Requested movement distance, for example 1 row or one page of rows. */
	int32_t step;
	aegisub_core_grid_modifier_state modifiers;
} aegisub_core_grid_keyboard_selection_input;

/* Selection policy input after one row was inserted. */
typedef struct aegisub_core_grid_row_insert_selection_input {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_grid_row_insert_selection_input). Zero is treated as
	 * the legacy zero-initialized caller size. */
	size_t struct_size;
	/* Row count after the insertion. inserted_row must be inside this range. */
	int32_t row_count;
	/* Zero-based index of the row inserted by the successful edit. */
	int32_t inserted_row;
} aegisub_core_grid_row_insert_selection_input;

/* Selection policy input after a contiguous row range was deleted. */
typedef struct aegisub_core_grid_rows_delete_selection_input {
	/* Must be AEGISUB_CORE_ABI_VERSION. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_grid_rows_delete_selection_input). Zero is treated as
	 * the legacy zero-initialized caller size. */
	size_t struct_size;
	/* Row count before deletion. The deleted range must be non-empty and stay
	 * within [0, row_count_before). */
	int32_t row_count_before;
	/* First zero-based row removed by the successful edit. */
	int32_t first_deleted_row;
	/* Number of contiguous rows removed by the successful edit. */
	int32_t deleted_row_count;
} aegisub_core_grid_rows_delete_selection_input;

/* Grid-selection plan returned by policy functions. Boolean flags tell the
 * host which pieces of GUI state should be updated; selected_row_count is for
 * sizing a batch read from the plan handle. */
typedef struct aegisub_core_grid_selection_plan_info {
	/* Non-zero when the policy recognized and handled the input. */
	uint8_t handled;
	/* Non-zero when active_row should replace the host active row. */
	uint8_t set_active;
	/* Non-zero when selected rows should replace the host selection. */
	uint8_t set_selection;
	/* Non-zero when media/subtitle preview activation should be triggered. */
	uint8_t activate_media;
	/* Non-zero when the host should scroll active_row into view. */
	uint8_t make_active_visible;
	/* Suggested active row. Meaningful when set_active is non-zero. */
	int32_t active_row;
	/* Suggested anchor row for future range selections. */
	int32_t anchor_row;
	/* Number of selected rows available through selected-row read APIs. */
	size_t selected_row_count;
} aegisub_core_grid_selection_plan_info;

/* Optional host-thread hooks. If any hook is supplied, post_to_main is required.
 * Core may use these to marshal callbacks/work to the GUI main thread without
 * depending on a specific GUI framework.
 *
 * post_to_main receives an opaque task pointer from core. It must either run
 * task(task_userdata) on the main thread before returning or enqueue it so it
 * will run later. The task pointer is core-owned. The contract on accept is
 * strict: after returning non-zero the host MUST invoke task(task_userdata)
 * exactly once (either inline before returning or later on the main executor).
 * Failure to do so leaks the task and any state it carries, and any
 * synchronous operation waiting on it (see synchronous_invoke_timeout_ms) will
 * block until it times out. Returning 0 rejects the work before any task state
 * is committed and lets core fail the operation that needed the marshal; in
 * that case core releases the task and the host must not invoke it.
 *
 * is_main_thread is optional. When supplied, it should return non-zero only on
 * the same thread/executor that runs posted tasks.
 *
 * flush_main_jobs is optional and intended for tests or hosts with an explicit
 * pump. It should execute queued core tasks that are ready to run and return
 * the number executed.
 *
 * host_userdata is never interpreted by core and is passed back unchanged to
 * every supplied hook. synchronous_invoke_timeout_ms is a host policy value for
 * operations that need synchronous main-thread work; zero means use the core
 * default. Setting only synchronous_invoke_timeout_ms still counts as supplying
 * hooks, so post_to_main must also be provided. */
typedef void (AEGISUB_CORE_CALL *aegisub_core_host_task)(void *task_userdata);
typedef uint8_t (AEGISUB_CORE_CALL *aegisub_core_host_post_to_main)(
	void *host_userdata,
	aegisub_core_host_task task,
	void *task_userdata);
typedef uint8_t (AEGISUB_CORE_CALL *aegisub_core_host_is_main_thread)(void *host_userdata);
typedef size_t (AEGISUB_CORE_CALL *aegisub_core_host_flush_main_jobs)(void *host_userdata);

typedef struct aegisub_core_host_thread_hooks {
	/* Opaque host pointer passed to each callback. May be NULL. */
	void *host_userdata;
	/* Required when any hook field is supplied. Must enqueue the provided task
	 * and eventually invoke it exactly once on the main executor, or return 0
	 * to reject it up front. Returning non-zero obligates the host to invoke
	 * the task exactly once; dropping an accepted task leaks it and stalls any
	 * synchronous operation waiting on it until timeout. */
	aegisub_core_host_post_to_main post_to_main;
	/* Optional fast-path check for the GUI/main executor. */
	aegisub_core_host_is_main_thread is_main_thread;
	/* Optional explicit pump hook for tests or hosts without an ambient loop. */
	aegisub_core_host_flush_main_jobs flush_main_jobs;
	/* Optional timeout in milliseconds for synchronous main-thread invokes.
	 * Zero keeps the core default. */
	uint32_t synchronous_invoke_timeout_ms;
} aegisub_core_host_thread_hooks;

/* Root context creation options. Pass NULL to aegisub_core_context_create() or
 * zero-initialize this struct, set abi_version, and set struct_size to
 * sizeof(this struct) for default behavior. struct_size enables tail-field
 * evolution; see "Struct evolution" in the header notes. Unknown future fields
 * must remain zero when using an older header/library pair. */
typedef struct aegisub_core_context_options {
	/* Must be AEGISUB_CORE_ABI_VERSION when this struct is passed. */
	uint32_t abi_version;
	/* Byte size of the struct the caller compiled against. Set to
	 * sizeof(aegisub_core_context_options). Zero is treated as the legacy
	 * zero-initialized caller size. */
	size_t struct_size;
	/* Optional callbacks for host-controlled main-thread dispatch. */
	aegisub_core_host_thread_hooks thread_hooks;
} aegisub_core_context_options;

/* Returns the ABI implemented by the loaded library. Check this before calling
 * other functions when the header and library may come from different builds. */
AEGISUB_CORE_API uint32_t AEGISUB_CORE_CALL aegisub_core_abi_version(void);
/* Frees aegisub_core_owned_string storage returned by this API. Safe for
 * {NULL, 0}; the value struct itself is passed by value and remains caller
 * storage. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_free_string(aegisub_core_owned_string value);

/* Create/destroy the root core context. A context owns host hooks, last_error,
 * and per-context provider diagnostics, but not the provider registry itself.
 *
 * Process-global registry semantics: the provider registry is shared across
 * every context in the process and is NOT owned by any single context. A new
 * context observes whatever registration/finalization state the process is in,
 * so a freshly created context may see an already-finalized registry. This
 * means the C ABI models a single-process, single-active-host embedding: do
 * not rely on per-context provider isolation, and do not embed two independent
 * core instances in one process expecting separate registries. The GUI
 * single-instance host use case is the intended consumer.
 *
 * Media/subtitle sessions are independent handles that must be destroyed with
 * their matching destroy function.
 *
 * On successful creation *context is non-NULL. On failure, when context itself
 * is non-NULL, *context is set to NULL before validation/work starts. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_context_create(aegisub_core_context **context);
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_context_create_with_options(
	aegisub_core_context_options const *options,
	aegisub_core_context **context);
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_context_destroy(aegisub_core_context *context);
/* Returns a copy of the last context error. The caller owns the returned string
 * and must release it with aegisub_core_free_string(). Passing NULL returns an
 * empty owned string, which is still safe to pass to aegisub_core_free_string().
 * Last-error text is diagnostic only; branch on aegisub_core_status values. */
AEGISUB_CORE_API aegisub_core_owned_string AEGISUB_CORE_CALL aegisub_core_context_last_error(aegisub_core_context *context);

/* Registers optional provider factories compiled into the core library, such
 * as AviSynth when WITH_AVISYNTH is enabled.
 *
 * This is explicit because provider selection is host startup policy: a wx,
 * Avalonia, CLI, or embedded host may choose which built-in and plugin
 * factories are visible before the registry is closed. The provider registry is
 * process-global, not owned by the context passed here. The context is used for
 * status diagnostics only.
 *
 * Single-process, single-host semantics: because the registry is shared across
 * all contexts in the process, registering from one context affects every
 * context. The intended consumer is one active host embedding per process.
 *
 * The call is idempotent while the registry is open. Call it before
 * aegisub_core_finalize_provider_registry(). If any context has already
 * finalized the process registry, this returns
 * AEGISUB_CORE_STATUS_REGISTRY_FINALIZED and leaves the registry unchanged. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_register_builtin_provider_factories(aegisub_core_context *context);
/* Finalizes all process-global provider registries.
 *
 * Finalization is one-way for the lifetime of the loaded core library: it
 * prevents late provider registration and makes catalog/open behavior stable
 * for subsequent media sessions. Hosts normally call this after registering
 * built-in factories and any host/plugin factories they want to expose. It is
 * safe to call more than once; later calls leave the finalized registry as-is.
 * Because the registry is process-global, finalizing from one context finalizes
 * it for every context in the process. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_finalize_provider_registry(aegisub_core_context *context);
/* Returns non-zero when the process-global provider registry has already been
 * finalized. Because the registry is process-global, this may return non-zero
 * for a newly created context if another context finalized it earlier. */
AEGISUB_CORE_API uint8_t AEGISUB_CORE_CALL aegisub_core_provider_registry_is_finalized(aegisub_core_context *context);

/* Provider catalogs are snapshots. Descriptor string views are borrowed from
 * the catalog handle; destroy the catalog after copying anything the host keeps.
 * preferred_provider may be empty.
 *
 * On success *catalog is non-NULL even when the catalog is empty. On failure,
 * when catalog itself is non-NULL, *catalog is set to NULL before validation. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_catalog_get(
	aegisub_core_context *context,
	aegisub_core_provider_kind kind,
	aegisub_core_string preferred_provider,
	aegisub_core_provider_catalog **catalog);
/* Returns zero for NULL catalog. */
AEGISUB_CORE_API size_t AEGISUB_CORE_CALL aegisub_core_provider_catalog_count(aegisub_core_provider_catalog const *catalog);
/* Single descriptor read. Prefer descriptors_get when reading more than one
 * provider across a managed/native boundary. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_catalog_get_at(
	aegisub_core_provider_catalog const *catalog,
	size_t index,
	aegisub_core_provider_descriptor *descriptor);
/* Batch descriptor read. first_provider may equal count for an empty tail
 * window. descriptors may be NULL only when provider_count is zero. *written is
 * set to zero before validation and to the number copied on success. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_catalog_descriptors_get(
	aegisub_core_provider_catalog const *catalog,
	size_t first_provider,
	size_t provider_count,
	aegisub_core_provider_descriptor *descriptors,
	size_t *written);
/* Releases the catalog snapshot. Safe to pass NULL. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_provider_catalog_destroy(aegisub_core_provider_catalog *catalog);

/* Last provider-open report for the context/kind. Attempt string views are
 * borrowed from the report handle. Use *_attempts_get for UI display to avoid
 * one call per attempt across a managed/native boundary. On success *report is
 * non-NULL; on failure, when report itself is non-NULL, *report is set to NULL
 * before validation. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_get(
	aegisub_core_context *context,
	aegisub_core_provider_kind kind,
	aegisub_core_provider_open_report **report);
/* Copies report metadata. String views are borrowed from the report handle and
 * become invalid when that report is destroyed. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_info_get(
	aegisub_core_provider_open_report const *report,
	aegisub_core_provider_open_report_info *info);
/* Returns zero for NULL report. */
AEGISUB_CORE_API size_t AEGISUB_CORE_CALL aegisub_core_provider_open_report_attempt_count(
	aegisub_core_provider_open_report const *report);
/* Single attempt read. Prefer attempts_get when showing more than one attempt
 * across a managed/native boundary. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_attempt_get_at(
	aegisub_core_provider_open_report const *report,
	size_t index,
	aegisub_core_provider_open_attempt *attempt);
/* Batch attempt read. first_attempt may equal count for an empty tail window.
 * attempts may be NULL only when attempt_count is zero. *written is set to zero
 * before validation and to the number copied on success. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_attempts_get(
	aegisub_core_provider_open_report const *report,
	size_t first_attempt,
	size_t attempt_count,
	aegisub_core_provider_open_attempt *attempts,
	size_t *written);
/* Releases the report snapshot. Safe to pass NULL. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_provider_open_report_destroy(
	aegisub_core_provider_open_report *report);

/* Open media sessions. Info string views are borrowed from the session; destroy
 * sessions with their matching destroy function. On failure, when session itself
 * is non-NULL, *session is set to NULL before validation/work starts. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_open(
	aegisub_core_context *context,
	aegisub_core_video_open_options const *options,
	aegisub_core_video_session **session);
/* Releases the video session. Safe to pass NULL. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_video_destroy(aegisub_core_video_session *session);
/* Copies video metadata. String views are borrowed from the video session and
 * become invalid when the session is destroyed. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_info_get(
	aegisub_core_video_session const *session,
	aegisub_core_video_info *info);
/* Reads metadata for a decoded CPU BGRA frame. This may decode the requested
 * frame; callers that immediately need pixels should follow with
 * aegisub_core_video_frame_bgra_get using data_size for the buffer length. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_frame_bgra_info_get(
	aegisub_core_video_session const *session,
	int32_t frame_number,
	aegisub_core_video_frame_info *info);
/* Decodes and copies one CPU BGRA frame into caller-owned storage. buffer must
 * have at least info_get.data_size bytes for the same frame. On success, info is
 * filled with the copied frame metadata. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_frame_bgra_get(
	aegisub_core_video_session const *session,
	int32_t frame_number,
	uint8_t *buffer,
	size_t buffer_size,
	aegisub_core_video_frame_info *info);
/* Batch keyframe reads are preferred for timeline consumers. */
AEGISUB_CORE_API size_t AEGISUB_CORE_CALL aegisub_core_video_keyframe_count(aegisub_core_video_session const *session);
/* Single keyframe read. Prefer keyframes_get for ranges. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_keyframe_get_at(
	aegisub_core_video_session const *session,
	size_t index,
	int32_t *keyframe);
/* Batch keyframe read. first_keyframe may equal count for an empty tail window.
 * keyframes may be NULL only when keyframe_count is zero. *written is set to
 * zero before validation and to the number copied on success. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_keyframes_get(
	aegisub_core_video_session const *session,
	size_t first_keyframe,
	size_t keyframe_count,
	int32_t *keyframes,
	size_t *written);

/* Open/query an audio session. Info string views are borrowed from the session;
 * destroy the session with aegisub_core_audio_destroy(). On failure, when
 * session itself is non-NULL, *session is set to NULL before validation/work
 * starts. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_audio_open(
	aegisub_core_context *context,
	aegisub_core_audio_open_options const *options,
	aegisub_core_audio_session **session);
/* Releases the audio session. Safe to pass NULL. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_audio_destroy(aegisub_core_audio_session *session);
/* Copies audio metadata. String views are borrowed from the audio session and
 * become invalid when the session is destroyed. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_audio_info_get(
	aegisub_core_audio_session const *session,
	aegisub_core_audio_info *info);

/* Subtitle sessions hold the loaded subtitle document. Query string views are
 * borrowed from the session; copy row strings before a successful row-edit API
 * call or before destroying the session if the host needs to keep them.
 * Saving clears dirty state but does not change row text storage. On open
 * failure, when session itself is non-NULL, *session is set to NULL before
 * validation/work starts. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_open(
	aegisub_core_context *context,
	aegisub_core_subtitle_open_options const *options,
	aegisub_core_subtitle_session **session);
/* Saves the subtitle document to options->path. On success dirty state is
 * cleared. On failure row data and dirty state are left unchanged. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_save(
	aegisub_core_context *context,
	aegisub_core_subtitle_session const *session,
	aegisub_core_subtitle_save_options const *options);
/* Releases the subtitle session and invalidates all borrowed subtitle strings.
 * Safe to pass NULL. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_subtitle_destroy(aegisub_core_subtitle_session *session);
/* Copies subtitle document metadata. String views are borrowed from the
 * subtitle session and become invalid when the session is destroyed. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_info_get(
	aegisub_core_subtitle_session const *session,
	aegisub_core_subtitle_info *info);
/* Coarse state read for GUI dirty indicators and row-cache invalidation. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_state_get(
	aegisub_core_subtitle_session const *session,
	aegisub_core_subtitle_state *state);
/* Current subtitle row count. Returns zero for NULL/invalid sessions, so use
 * subtitle_info_get/stateful status-returning calls when zero must be
 * distinguished from an invalid handle. */
AEGISUB_CORE_API size_t AEGISUB_CORE_CALL aegisub_core_subtitle_row_count(
	aegisub_core_subtitle_session const *session);
/* Single row read. Prefer rows_get for visible windows or bulk refresh. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_get_at(
	aegisub_core_subtitle_session const *session,
	size_t index,
	aegisub_core_subtitle_row *row);
/* Windowed row read for virtualized grids. first_row may equal the session row
 * count to read an empty tail window; values beyond the tail are invalid. rows
 * may be NULL only when row_count is zero. *written is set to zero before
 * validation and to the number copied on success. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_rows_get(
	aegisub_core_subtitle_session const *session,
	size_t first_row,
	size_t row_count,
	aegisub_core_subtitle_row *rows,
	size_t *written);
/* Mutating subtitle APIs validate inputs before editing. On failure they apply
 * no changes, set *applied to 0 where present, and do not increment revision.
 * Successful edits that leave row data unchanged are no-ops and do not dirty
 * the session. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_text_set(
	aegisub_core_subtitle_session *session,
	size_t row,
	aegisub_core_string text);
/* Batch text edit. edits may be NULL only when edit_count is zero. Edit rows
 * must be unique and in range. *applied receives the number of edits that
 * changed row text; an all-same-text batch is a successful no-op. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_texts_set(
	aegisub_core_subtitle_session *session,
	aegisub_core_subtitle_row_text_edit const *edits,
	size_t edit_count,
	size_t *applied);
/* Insert a blank dialogue row before row index position. position may equal
 * aegisub_core_subtitle_row_count(session) to append. The inserted row uses the
 * neighboring row style/timing as a template where possible, has a fresh line
 * id, and has empty text. On success *inserted_row receives the new row index.
 * This invalidates all borrowed row string views and may shift row indexes. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_insert(
	aegisub_core_subtitle_session *session,
	size_t position,
	size_t *inserted_row);
/* Delete a contiguous row range. first_row + row_count must stay within the
 * current row count. row_count == 0 is a successful no-op and does not change
 * revision; otherwise *deleted receives the number of removed rows. Deleting
 * rows invalidates all borrowed row string views and may shift row indexes. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_rows_delete(
	aegisub_core_subtitle_session *session,
	size_t first_row,
	size_t row_count,
	size_t *deleted);
/* Move the selected rows one row up (direction < 0) or down (direction > 0).
 * selected_rows are zero-based row indexes in the current subtitle session and
 * must be unique and in range; the operation is all-or-none. selected_rows may
 * be NULL only when selected_row_count is zero. moved_selected_rows must have
 * room for selected_row_count entries and may be NULL only when
 * selected_row_count is zero. On success with an actual move, the output array
 * receives the moved selected rows in display order and
 * *moved_selected_row_count receives selected_row_count. If the selection is
 * already at the requested edge, the call succeeds as a no-op, writes count 0,
 * and does not change revision/dirty state. Moving rows invalidates all
 * borrowed row string views and shifts row indexes in the changed window. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_selected_rows_move(
	aegisub_core_subtitle_session *session,
	int32_t const *selected_rows,
	size_t selected_row_count,
	int32_t direction,
	int32_t *moved_selected_rows,
	size_t *moved_selected_row_count);
/* Duplicate selected rows, preserving Aegisub's block behavior: each
 * contiguous selected row block is copied immediately after that block.
 * selected_rows are zero-based row indexes in the current subtitle session and
 * must be unique and in range; the operation is all-or-none. selected_rows may
 * be NULL only when selected_row_count is zero. duplicated_rows must have room
 * for selected_row_count entries and may be NULL only when selected_row_count
 * is zero. On success with an actual duplicate, duplicated_rows receives the
 * new copied row indexes in display order and *duplicated_row_count receives
 * selected_row_count. Empty selection succeeds as a no-op, writes count 0, and
 * does not change revision/dirty state. Duplicating rows invalidates all
 * borrowed row string views and shifts row indexes in the changed window. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_selected_rows_duplicate(
	aegisub_core_subtitle_session *session,
	int32_t const *selected_rows,
	size_t selected_row_count,
	int32_t *duplicated_rows,
	size_t *duplicated_row_count);
/* Sort subtitle rows by one of aegisub_core_subtitle_sort_key.
 *
 * If selected_only is zero, the whole subtitle row list is sorted and
 * selected_rows/sorted_selected_rows are ignored; sorted_selected_row_count is
 * still required and is set to zero. If selected_only is non-zero, selected_rows
 * must contain unique in-range row indexes. The selected rows are sorted with
 * Aegisub's selected-sort behavior: each contiguous selected block is sorted in
 * place while unselected rows stay outside that block. On an actual selected
 * sort, sorted_selected_rows must have room for selected_row_count entries and
 * receives the sorted selected row indexes in display order. For empty or
 * single-row selected sorts, the call succeeds as a no-op, writes count 0, and
 * does not change revision/dirty state. Sorting invalidates all borrowed row
 * string views when it changes row order. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_rows_sort(
	aegisub_core_subtitle_session *session,
	aegisub_core_subtitle_sort_key key,
	uint8_t selected_only,
	int32_t const *selected_rows,
	size_t selected_row_count,
	int32_t *sorted_selected_rows,
	size_t *sorted_selected_row_count);
/* Paste source rows over target rows using the selected paste-over fields.
 * target_rows and sources must have exactly row_count entries and are applied
 * pairwise. target_rows must contain unique in-range row indexes. Only source
 * fields selected by fields are read. fields == 0 or row_count == 0 succeeds as
 * a no-op, writes *applied = 0, and does not change revision/dirty state; when
 * row_count is non-zero the target_rows and sources arrays must still be
 * non-NULL, target row validation still applies, but unselected source fields
 * are ignored. The operation is all-or-none: invalid target rows, duplicate
 * targets, unknown field bits, negative selected times/margins, or start > end
 * in the resulting row reject the whole batch. *applied receives the number of
 * target rows whose selected fields changed; an all-same-data batch is a
 * successful no-op. Successful paste-over invalidates borrowed row string
 * views for the changed rows. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_paste_over_apply(
	aegisub_core_subtitle_session *session,
	int32_t const *target_rows,
	aegisub_core_subtitle_paste_over_source const *sources,
	size_t row_count,
	aegisub_core_subtitle_paste_over_fields fields,
	size_t *applied);
/* Prefer this batch patch API for editor integrations. It keeps the
 * cross-language boundary coarse-grained while still validating edits before
 * touching the subtitle model. patches may be NULL only when patch_count is
 * zero. Patch rows must be unique and in range. *applied receives the number of
 * patches whose selected fields changed; a non-empty all-zero-field or
 * all-same-data batch is a successful no-op. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_patches_apply(
	aegisub_core_subtitle_session *session,
	aegisub_core_subtitle_row_patch const *patches,
	size_t patch_count,
	size_t *applied);

/* Stateless grid-selection planning. The returned plan is a snapshot owned by
 * the plan handle. Use selected_rows_get for batch transfer to managed hosts.
 * Destroy every successful plan with aegisub_core_grid_selection_plan_destroy().
 * On failure, when plan itself is non-NULL, *plan is set to NULL before
 * validation/work starts. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_mouse(
	aegisub_core_grid_mouse_selection_input const *input,
	aegisub_core_grid_selection_plan **plan);
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_keyboard(
	aegisub_core_grid_keyboard_selection_input const *input,
	aegisub_core_grid_selection_plan **plan);
/* Stateless selection suggestions for row-count-changing edits. Use these
 * after a successful subtitle row insert/delete so another GUI can update
 * active row, anchor, selected rows, and scroll visibility without duplicating
 * Aegisub grid policy. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_row_insert(
	aegisub_core_grid_row_insert_selection_input const *input,
	aegisub_core_grid_selection_plan **plan);
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_rows_delete(
	aegisub_core_grid_rows_delete_selection_input const *input,
	aegisub_core_grid_selection_plan **plan);
/* Copies selection-plan metadata. Use selected_row_count to size a following
 * selected_rows_get batch call. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_info_get(
	aegisub_core_grid_selection_plan const *plan,
	aegisub_core_grid_selection_plan_info *info);
/* Returns zero for NULL plan. */
AEGISUB_CORE_API size_t AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_selected_row_count(
	aegisub_core_grid_selection_plan const *plan);
/* Single selected-row read. Prefer selected_rows_get for whole selections. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_selected_row_get_at(
	aegisub_core_grid_selection_plan const *plan,
	size_t index,
	int32_t *row);
/* Batch selected-row read. first_row may equal count for an empty tail window.
 * rows may be NULL only when row_count is zero. *written is set to zero before
 * validation and to the number copied on success. */
AEGISUB_CORE_API aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_selected_rows_get(
	aegisub_core_grid_selection_plan const *plan,
	size_t first_row,
	size_t row_count,
	int32_t *rows,
	size_t *written);
/* Releases the selection-plan snapshot. Safe to pass NULL. */
AEGISUB_CORE_API void AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_destroy(
	aegisub_core_grid_selection_plan *plan);

#ifdef __cplusplus
}
#endif
