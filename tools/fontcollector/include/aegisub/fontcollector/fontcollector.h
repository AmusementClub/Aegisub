// Copyright (c) 2026, MIRIMIRIM

#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(AEGISUB_FONTCOLLECTOR_STATIC)
#define AEGISUB_FONTCOLLECTOR_API
#elif defined(_WIN32)
#if defined(AEGISUB_FONTCOLLECTOR_BUILD)
#define AEGISUB_FONTCOLLECTOR_API __declspec(dllexport)
#else
#define AEGISUB_FONTCOLLECTOR_API __declspec(dllimport)
#endif
#else
#define AEGISUB_FONTCOLLECTOR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AegisubFontCollectorMode {
	AEGISUB_FONTCOLLECTOR_MODE_CHECK = 0,
	AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER = 1,
	AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER = 2
} AegisubFontCollectorMode;

typedef enum AegisubFontCollectorMatcher {
	AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM = 0,
	AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS = 1
} AegisubFontCollectorMatcher;

typedef enum AegisubFontCollectorResult {
	AEGISUB_FONTCOLLECTOR_OK = 0,
	AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT = 1,
	AEGISUB_FONTCOLLECTOR_INVALID_MODE = 2,
	AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION = 3,
	AEGISUB_FONTCOLLECTOR_RESULT_RESERVED_4 = 4,
	AEGISUB_FONTCOLLECTOR_READ_FAILED = 5,
	AEGISUB_FONTCOLLECTOR_COLLECT_FAILED = 6
} AegisubFontCollectorResult;

typedef enum AegisubFontCollectorMatchStatus {
	AEGISUB_FONTCOLLECTOR_MATCH_FOUND = 0,
	AEGISUB_FONTCOLLECTOR_MATCH_MISSING = 1,
	AEGISUB_FONTCOLLECTOR_MATCH_MEMORY_ONLY = 2
} AegisubFontCollectorMatchStatus;

typedef enum AegisubFontCollectorEventType {
	AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO,
	AEGISUB_FONTCOLLECTOR_EVENT_UPDATING_FONT_CACHE,
	AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR,
	AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE,
	AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING,
	AEGISUB_FONTCOLLECTOR_EVENT_SEARCHING_FOR_FONT_FILES,
	AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING,
	AEGISUB_FONTCOLLECTOR_EVENT_FONT_FOUND,
	AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD,
	AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC,
	AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS,
	AEGISUB_FONTCOLLECTOR_EVENT_USAGE,
	AEGISUB_FONTCOLLECTOR_EVENT_SEARCH_COMPLETE,
	AEGISUB_FONTCOLLECTOR_EVENT_ALL_FONTS_FOUND,
	AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING,
	AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING_GLYPHS,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_FOLDER,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_ARCHIVE,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPIED,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_ALREADY_EXISTS,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_ALL_COPIED,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED,
	AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE
} AegisubFontCollectorEventType;

typedef struct AegisubFontCollectorRequest {
	/* Paths and encoding names are UTF-8 strings. */
	char const *input_path;
	char const *destination_path;
	char const *encoding;
	AegisubFontCollectorMode mode;
} AegisubFontCollectorRequest;

typedef struct AegisubFontCollectorSummary {
	size_t font_usage_count;
	size_t found_font_count;
	size_t missing_style_count;
	size_t missing_font_count;
	size_t missing_glyph_font_count;
	size_t fake_bold_count;
	size_t fake_italic_count;
	uint64_t copied_font_count;
	uint64_t collection_failure_count;
} AegisubFontCollectorSummary;

typedef struct AegisubFontCollectorEvent {
	AegisubFontCollectorEventType type;
	char const *face;
	char const *message;
	char const *style;
	char const *path;
	char const *const *styles;
	size_t style_count;
	int const *lines;
	size_t line_count;
	int count;
	int requested_weight;
	int requested_italic;
} AegisubFontCollectorEvent;

typedef struct AegisubFontCollectorMatchedFont {
	AegisubFontCollectorMatchStatus match_status;
	char const *facename;
	int face_index;
	int weight;
	int bold;
	int italic;
	char const *path_source;
	char const *const *paths;
	size_t path_count;
	int fake_bold;
	int fake_italic;
	char const *missing_text;
	uint32_t const *missing_codepoints;
	size_t missing_codepoint_count;
	int requested_weight;
	/* Source line numbers containing missing glyphs; falls back to dialogue rows for in-memory scripts. */
	int const *missing_lines;
	size_t missing_line_count;
} AegisubFontCollectorMatchedFont;

typedef struct AegisubFontCollectorMatchCandidate {
	char const *facename;
	char const *facename_full;
	char const *matched_name;
	/* "requested", "default", or "fallback". */
	char const *match_source;
	/* Family/full-name/PostScript match category used by the selector. */
	char const *name_match;
	char const *path;
	/* Zero-based order in the provider catalog; libass uses this to break equal scores. */
	int provider_order;
	int face_index;
	/* libass attribute distance; lower values are preferred. */
	int score;
	int weight;
	int bold;
	int italic;
	uint32_t const *considered_codepoints;
	size_t considered_codepoint_count;
	uint32_t const *supported_codepoints;
	size_t supported_codepoint_count;
	uint32_t const *selected_codepoints;
	size_t selected_codepoint_count;
} AegisubFontCollectorMatchCandidate;

typedef struct AegisubFontCollectorFontUsage {
	char const *ass_facename;
	int ass_bold;
	int ass_italic;
	uint32_t const *codepoints;
	size_t codepoint_count;
	char const *const *styles;
	size_t style_count;
	int const *override_lines;
	size_t override_line_count;
	AegisubFontCollectorMatchedFont matched;
	/* Source line numbers using this font request; falls back to dialogue rows for in-memory scripts. */
	int const *lines;
	size_t line_count;
	/* Diagnostic only: full realized matched face name when the platform can report it. */
	char const *matched_facename_full;
	/* Diagnostic only: matched family aliases reported by the backend. */
	char const *const *matched_names;
	size_t matched_name_count;
	/* Diagnostic-only libass candidate evidence. Pointers are callback-scoped. */
	AegisubFontCollectorMatchCandidate const *match_candidates;
	size_t match_candidate_count;
	/* True when equal-score candidates could serve at least one selected codepoint. */
	int match_ambiguous;
} AegisubFontCollectorFontUsage;

typedef struct AegisubFontCollectorSession AegisubFontCollectorSession;

typedef struct AegisubFontCollectorSessionOptions {
	size_t struct_size;
	AegisubFontCollectorMatcher matcher;
	/* UTF-8 font file paths. Directories must be expanded by the caller. */
	char const *const *additional_font_files;
	size_t additional_font_file_count;
	/*
	 * Non-zero: include the system font catalog.
	 * Zero: private catalog only; additional_font_file_count must be > 0 for
	 * the libass matcher. Zero-init of this struct leaves the field at 0, so
	 * callers of session_create_with_options must set include_system_fonts
	 * explicitly (use 1 unless building a private-font catalog).
	 */
	int include_system_fonts;
	int collect_match_candidates;
} AegisubFontCollectorSessionOptions;

#define AEGISUB_FONTCOLLECTOR_SESSION_OPTIONS_V1_SIZE \
	(offsetof(AegisubFontCollectorSessionOptions, collect_match_candidates) + \
	 sizeof(((AegisubFontCollectorSessionOptions *)0)->collect_match_candidates))

/* Event pointer fields are valid only for the duration of the callback. */
typedef void (*AegisubFontCollectorEventCallback)(AegisubFontCollectorEvent const *event, void *user_data);
/* Usage pointer fields are valid only for the duration of the callback. */
typedef void (*AegisubFontCollectorFontUsageCallback)(AegisubFontCollectorFontUsage const *usage, void *user_data);

typedef struct AegisubFontCollectorBatchItem {
	AegisubFontCollectorRequest request;
	AegisubFontCollectorEventCallback event_callback;
	void *event_user_data;
	AegisubFontCollectorFontUsageCallback usage_callback;
	void *usage_user_data;
	AegisubFontCollectorSummary *summary;
	char *error_buffer;
	size_t error_buffer_size;
	int result;
} AegisubFontCollectorBatchItem;

typedef enum AegisubFontNameNormalizationTarget {
	AEGISUB_FONT_NAME_TARGET_LOCALIZED = 0,
	AEGISUB_FONT_NAME_TARGET_ENGLISH_WIN32 = 1
} AegisubFontNameNormalizationTarget;

typedef enum AegisubFontNameSourceKind {
	AEGISUB_FONT_NAME_SOURCE_STYLE = 0,
	AEGISUB_FONT_NAME_SOURCE_OVERRIDE = 1
} AegisubFontNameSourceKind;

typedef enum AegisubFontFamilyMatchKind {
	AEGISUB_FONT_FAMILY_MATCH_NONE = 0,
	AEGISUB_FONT_FAMILY_MATCH_EXACT = 1,
	AEGISUB_FONT_FAMILY_MATCH_CASE_INSENSITIVE_EXACT = 2,
	AEGISUB_FONT_FAMILY_MATCH_AMBIGUOUS = 3
} AegisubFontFamilyMatchKind;

/* Versioned, read-only font-name normalization API. */
typedef struct AegisubFontNameNormalizationRequest {
	size_t struct_size;
	/* Paths and encoding names are UTF-8 strings. */
	char const *input_path;
	char const *encoding;
	AegisubFontNameNormalizationTarget target;
} AegisubFontNameNormalizationRequest;

#define AEGISUB_FONT_NAME_NORMALIZATION_REQUEST_V1_SIZE \
	(offsetof(AegisubFontNameNormalizationRequest, target) + \
	 sizeof(((AegisubFontNameNormalizationRequest *)0)->target))

typedef struct AegisubFontNameNormalizationChange {
	size_t struct_size;
	AegisubFontNameSourceKind source_kind;
	char const *style;
	int line;
	size_t override_index;
	int comment;
	char const *current_name;
	char const *recommended_name;
	AegisubFontFamilyMatchKind match_kind;
	char const *reason_code;
	int safe_to_apply;
} AegisubFontNameNormalizationChange;

#define AEGISUB_FONT_NAME_NORMALIZATION_CHANGE_V1_SIZE \
	(offsetof(AegisubFontNameNormalizationChange, safe_to_apply) + \
	 sizeof(((AegisubFontNameNormalizationChange *)0)->safe_to_apply))

typedef struct AegisubFontNameNormalizationSummary {
	size_t struct_size;
	int catalog_available;
	size_t scanned_name_count;
	size_t finding_count;
	size_t safe_change_count;
	size_t unsafe_finding_count;
} AegisubFontNameNormalizationSummary;

#define AEGISUB_FONT_NAME_NORMALIZATION_SUMMARY_V1_SIZE \
	(offsetof(AegisubFontNameNormalizationSummary, unsafe_finding_count) + \
	 sizeof(((AegisubFontNameNormalizationSummary *)0)->unsafe_finding_count))

/* Change pointer fields are valid only for the duration of the callback. */
typedef void (*AegisubFontNameNormalizationCallback)(
	AegisubFontNameNormalizationChange const *change,
	void *user_data);

typedef struct AegisubFontNameNormalizationBatchItem {
	size_t struct_size;
	AegisubFontNameNormalizationRequest const *request;
	AegisubFontNameNormalizationCallback callback;
	void *user_data;
	AegisubFontNameNormalizationSummary *summary;
	char *error_buffer;
	size_t error_buffer_size;
	int result;
} AegisubFontNameNormalizationBatchItem;

#define AEGISUB_FONT_NAME_NORMALIZATION_BATCH_ITEM_V1_SIZE \
	(offsetof(AegisubFontNameNormalizationBatchItem, result) + \
	 sizeof(((AegisubFontNameNormalizationBatchItem *)0)->result))

AEGISUB_FONTCOLLECTOR_API int aegisub_fontcollector_session_create_with_options(
	AegisubFontCollectorSessionOptions const *options,
	AegisubFontCollectorEventCallback callback,
	void *user_data,
	AegisubFontCollectorSession **session,
	char *error_buffer,
	size_t error_buffer_size);

/* Resolves merged font requests/codepoints across all valid items, then reports per item. */
AEGISUB_FONTCOLLECTOR_API int aegisub_fontcollector_session_collect_batch(
	AegisubFontCollectorSession *session,
	AegisubFontCollectorBatchItem *items,
	size_t item_count,
	char *error_buffer,
	size_t error_buffer_size);

AEGISUB_FONTCOLLECTOR_API void aegisub_fontcollector_session_destroy(
	AegisubFontCollectorSession *session);

/* Builds a plan only; never modifies input_path. */
AEGISUB_FONTCOLLECTOR_API int aegisub_fontcollector_build_normalization_plan(
	AegisubFontNameNormalizationRequest const *request,
	AegisubFontNameNormalizationCallback callback,
	void *user_data,
	AegisubFontNameNormalizationSummary *summary,
	char *error_buffer,
	size_t error_buffer_size);

/* Builds all plans against one immutable font-family catalog snapshot. */
AEGISUB_FONTCOLLECTOR_API int aegisub_fontcollector_build_normalization_plan_batch(
	AegisubFontNameNormalizationBatchItem *const *items,
	size_t item_count,
	char *error_buffer,
	size_t error_buffer_size);

#ifdef __cplusplus
}
#endif
