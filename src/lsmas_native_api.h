#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

typedef struct lsmas_handle_t lsmas_handle_t;
typedef struct lsmas_video_frame_t lsmas_video_frame_t;

typedef int (*lsmas_progress_callback_t)(void *userdata, const char *message_utf8, int32_t percent);

typedef enum lsmas_seek_mode_t {
    LSMAS_SEEK_NORMAL = 0,
    LSMAS_SEEK_UNSAFE = 1,
    LSMAS_SEEK_AGGRESSIVE = 2
} lsmas_seek_mode_t;

typedef enum lsmas_hw_pref_t {
    LSMAS_HW_NONE = 0,
    LSMAS_HW_CUVID = 1,
    LSMAS_HW_QSV = 2,
    LSMAS_HW_CUVID_THEN_QSV = 3
} lsmas_hw_pref_t;

typedef enum lsmas_field_dominance_t {
    LSMAS_DOMINANCE_OBEY = 0,
    LSMAS_DOMINANCE_TFF = 1,
    LSMAS_DOMINANCE_BFF = 2
} lsmas_field_dominance_t;

typedef struct lsmas_video_open_options_t {
    int32_t stream_index;
    int32_t threads;
    lsmas_seek_mode_t seek_mode;
    int32_t seek_threshold;
    int32_t direct_rendering;
    int32_t fpsnum;
    int32_t fpsden;
    int32_t variable_info;
    const char *output_format;
    const char *decoder;
    lsmas_hw_pref_t prefer_hw;
    int32_t ff_loglevel;
    int32_t cache_index;
    const char *cachefile;
    const char *cachedir;
    int32_t soft_reset;
    int32_t framelist;
    int32_t repeat;
    lsmas_field_dominance_t dominance;
} lsmas_video_open_options_t;

typedef enum lsmas_audio_sample_format_t {
    LSMAS_AUDIO_F32 = 0,
    LSMAS_AUDIO_S16 = 1,
    LSMAS_AUDIO_S32 = 2
} lsmas_audio_sample_format_t;

typedef struct lsmas_audio_open_options_t {
    int32_t stream_index;
    int32_t threads;
    int32_t av_sync;
    int32_t ff_loglevel;
    const char *decoder;
    int32_t cache_index;
    const char *cachefile;
    const char *cachedir;
    uint64_t channel_layout;
    int32_t sample_rate;
    lsmas_audio_sample_format_t sample_format;
} lsmas_audio_open_options_t;

typedef struct lsmas_video_info_t {
    int32_t width;
    int32_t height;
    int32_t num_frames;
    int32_t fps_num;
    int32_t fps_den;
} lsmas_video_info_t;

typedef struct lsmas_audio_info_t {
    int32_t stream_index;
    int32_t sample_rate;
    int32_t channels;
    uint64_t channel_layout;
    int32_t sample_format;
    int32_t bits_per_sample;
    int32_t bytes_per_sample;
    int32_t block_align;
    int64_t decoded_samples;
    int64_t delay_samples;
    int64_t total_samples;
} lsmas_audio_info_t;

typedef struct lsmas_video_props_t {
    int32_t sar_num;
    int32_t sar_den;
    int32_t color_range;
    int32_t colorspace;
    int32_t color_primaries;
    int32_t color_trc;
    int32_t chroma_location;
    int32_t field_order;
    int32_t interlaced_frame;
    int32_t top_field_first;
} lsmas_video_props_t;

typedef enum lsmas_video_color_family_t {
    LSMAS_COLOR_FAMILY_UNKNOWN = 0,
    LSMAS_COLOR_FAMILY_RGB = 1,
    LSMAS_COLOR_FAMILY_YCBCR = 2
} lsmas_video_color_family_t;

typedef struct lsmas_video_plane_format_t {
    int32_t width_divisor;
    int32_t height_divisor;
    int32_t components_per_sample;
    int32_t bytes_per_sample;
    int32_t bits_per_component;
    int32_t component_shift[4];
} lsmas_video_plane_format_t;

typedef struct lsmas_video_format_info_t {
    lsmas_video_color_family_t color_family;
    int32_t plane_count;
    lsmas_video_plane_format_t planes[4];
} lsmas_video_format_info_t;

typedef struct lsmas_video_frame_props_t {
    int32_t width;
    int32_t height;
    int32_t pix_fmt;
    int32_t plane_count;
    int32_t sar_num;
    int32_t sar_den;
    int32_t color_range;
    int32_t colorspace;
    int32_t color_primaries;
    int32_t color_trc;
    int32_t chroma_location;
    int32_t field_order;
    int32_t interlaced_frame;
    int32_t top_field_first;
    int32_t repeat_pict;
    int32_t crop_left;
    int32_t crop_top;
    int32_t crop_right;
    int32_t crop_bottom;
    int32_t display_rotation_degrees;
    int32_t display_hflip;
    int32_t display_vflip;
    int32_t has_mastering_display_metadata;
    int32_t has_content_light_metadata;
    int32_t has_dynamic_hdr_plus;
    int32_t has_dovi_metadata;
    int32_t has_dovi_rpu;
    int32_t has_film_grain_params;
    int32_t has_displaymatrix;
} lsmas_video_frame_props_t;

typedef enum lsmas_video_frame_side_data_type_t {
    LSMAS_FRAME_SIDE_DATA_MASTERING_DISPLAY_METADATA = 1,
    LSMAS_FRAME_SIDE_DATA_CONTENT_LIGHT_METADATA     = 2,
    LSMAS_FRAME_SIDE_DATA_DYNAMIC_HDR_PLUS           = 3,
    LSMAS_FRAME_SIDE_DATA_DOVI_METADATA              = 4,
    LSMAS_FRAME_SIDE_DATA_DOVI_RPU                   = 5,
    LSMAS_FRAME_SIDE_DATA_FILM_GRAIN_PARAMS          = 6,
    LSMAS_FRAME_SIDE_DATA_DISPLAYMATRIX              = 7
} lsmas_video_frame_side_data_type_t;

extern "C" {
char *lsmas_probe_streams_json_utf8(const char *file_path_utf8, char **error_message);
char *lsmas_get_versions_json_utf8(char **error_message);
int lsmas_video_get_time_base(lsmas_handle_t *handle, int32_t *out_num, int32_t *out_den, char **error_message);
int32_t lsmas_video_get_pts_list(lsmas_handle_t *handle, int64_t *out_pts, int32_t out_count, char **error_message);
int32_t lsmas_video_get_source_frame_count(lsmas_handle_t *handle, int32_t *out_count, char **error_message);
int32_t lsmas_video_get_source_keyframe_flags(lsmas_handle_t *handle, uint8_t *out_flags, int32_t out_count, char **error_message);
lsmas_handle_t *lsmas_video_open_with_progress_utf8(const char *file_path_utf8, const lsmas_video_open_options_t *options, lsmas_progress_callback_t progress_cb, void *progress_userdata, char **error_message);
void lsmas_video_close(lsmas_handle_t *handle);
int lsmas_video_get_info(lsmas_handle_t *handle, lsmas_video_info_t *out_info, char **error_message);
int lsmas_video_get_stream_props(lsmas_handle_t *handle, lsmas_video_props_t *out_props, char **error_message);
int64_t lsmas_video_get_frame_bgra(lsmas_handle_t *handle, int32_t frame_index, uint8_t *dst, int32_t dst_stride, char **error_message);
int lsmas_video_acquire_avframe(lsmas_handle_t *handle, int32_t frame_index, lsmas_video_frame_t **out_frame, char **error_message);
const void *lsmas_video_frame_get_avframe(const lsmas_video_frame_t *frame);
const char *lsmas_video_frame_get_pix_fmt_name(const lsmas_video_frame_t *frame);
int lsmas_video_frame_get_props(const lsmas_video_frame_t *frame, lsmas_video_frame_props_t *out_props, char **error_message);
int lsmas_video_frame_get_format_info(const lsmas_video_frame_t *frame, lsmas_video_format_info_t *out_info, char **error_message);
int lsmas_video_frame_get_plane(const lsmas_video_frame_t *frame, int32_t plane_index, const uint8_t **out_data, int32_t *out_stride, int32_t *out_width, int32_t *out_height, char **error_message);
int lsmas_video_frame_get_side_data(const lsmas_video_frame_t *frame, lsmas_video_frame_side_data_type_t type, const uint8_t **out_data, int32_t *out_size, char **error_message);
void lsmas_video_release_frame(lsmas_video_frame_t *frame);
lsmas_handle_t *lsmas_audio_open_with_progress_utf8(const char *file_path_utf8, const lsmas_audio_open_options_t *options, lsmas_progress_callback_t progress_cb, void *progress_userdata, char **error_message);
void lsmas_audio_close(lsmas_handle_t *handle);
int lsmas_audio_get_info(lsmas_handle_t *handle, lsmas_audio_info_t *out_info, char **error_message);
int64_t lsmas_audio_get_samples(lsmas_handle_t *handle, void *dst, int64_t start, int64_t wanted_length, char **error_message);
void lsmas_free(void *p);
}

namespace lsmas {

struct Api {
    decltype(&lsmas_probe_streams_json_utf8) probe_streams_json_utf8 = nullptr;
    decltype(&lsmas_get_versions_json_utf8) get_versions_json_utf8 = nullptr;
    decltype(&lsmas_video_open_with_progress_utf8) video_open_with_progress_utf8 = nullptr;
    decltype(&lsmas_video_close) video_close = nullptr;
    decltype(&lsmas_video_get_info) video_get_info = nullptr;
    decltype(&lsmas_video_get_stream_props) video_get_stream_props = nullptr;
    decltype(&lsmas_video_get_time_base) video_get_time_base = nullptr;
    decltype(&lsmas_video_get_pts_list) video_get_pts_list = nullptr;
    decltype(&lsmas_video_get_source_frame_count) video_get_source_frame_count = nullptr;
    decltype(&lsmas_video_get_source_keyframe_flags) video_get_source_keyframe_flags = nullptr;
    decltype(&lsmas_video_get_frame_bgra) video_get_frame_bgra = nullptr;
    decltype(&lsmas_video_acquire_avframe) video_acquire_avframe = nullptr;
    decltype(&lsmas_video_frame_get_avframe) video_frame_get_avframe = nullptr;
    decltype(&lsmas_video_frame_get_pix_fmt_name) video_frame_get_pix_fmt_name = nullptr;
    decltype(&lsmas_video_frame_get_props) video_frame_get_props = nullptr;
    decltype(&lsmas_video_frame_get_format_info) video_frame_get_format_info = nullptr;
    decltype(&lsmas_video_frame_get_plane) video_frame_get_plane = nullptr;
    decltype(&lsmas_video_frame_get_side_data) video_frame_get_side_data = nullptr;
    decltype(&lsmas_video_release_frame) video_release_frame = nullptr;
    decltype(&lsmas_audio_open_with_progress_utf8) audio_open_with_progress_utf8 = nullptr;
    decltype(&lsmas_audio_close) audio_close = nullptr;
    decltype(&lsmas_audio_get_info) audio_get_info = nullptr;
    decltype(&lsmas_audio_get_samples) audio_get_samples = nullptr;
    decltype(&lsmas_free) free = nullptr;
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
Api const& GetApi();

}
