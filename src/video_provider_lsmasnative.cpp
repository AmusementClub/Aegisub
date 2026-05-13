#ifdef WITH_LSMASNATIVE

#include "include/aegisub/video_provider.h"

#include "lsmas_native_api.h"
#include "lsmas_provider_common.h"
#include "options.h"
#include "source_frame.h"
#include "video_frame.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
enum AvColorSpace {
    AVCOL_SPC_RGB = 0,
    AVCOL_SPC_BT709 = 1,
    AVCOL_SPC_UNSPECIFIED = 2,
    AVCOL_SPC_FCC = 4,
    AVCOL_SPC_BT470BG = 5,
    AVCOL_SPC_SMPTE170M = 6,
    AVCOL_SPC_SMPTE240M = 7,
    AVCOL_SPC_YCOCG = 8,
    AVCOL_SPC_BT2020_NCL = 9,
    AVCOL_SPC_BT2020_CL = 10,
    AVCOL_SPC_ICTCP = 14
};

enum AvColorRange {
    AVCOL_RANGE_UNSPECIFIED = 0,
    AVCOL_RANGE_MPEG = 1,
    AVCOL_RANGE_JPEG = 2
};

enum AvColorPrimaries {
    AVCOL_PRI_BT709 = 1,
    AVCOL_PRI_BT470M = 4,
    AVCOL_PRI_BT470BG = 5,
    AVCOL_PRI_SMPTE170M = 6,
    AVCOL_PRI_SMPTE240M = 7,
    AVCOL_PRI_FILM = 8,
    AVCOL_PRI_BT2020 = 9,
    AVCOL_PRI_SMPTE431 = 11,
    AVCOL_PRI_SMPTE432 = 12
};

enum AvTransferCharacteristics {
    AVCOL_TRC_BT709 = 1,
    AVCOL_TRC_GAMMA22 = 4,
    AVCOL_TRC_GAMMA28 = 5,
    AVCOL_TRC_SMPTE170M = 6,
    AVCOL_TRC_SMPTE240M = 7,
    AVCOL_TRC_LINEAR = 8,
    AVCOL_TRC_IEC61966_2_1 = 13,
    AVCOL_TRC_BT2020_10 = 14,
    AVCOL_TRC_BT2020_12 = 15,
    AVCOL_TRC_SMPTE2084 = 16,
    AVCOL_TRC_ARIB_STD_B67 = 18
};

enum AvChromaLocation {
    AVCHROMA_LOC_LEFT = 1,
    AVCHROMA_LOC_CENTER = 2,
    AVCHROMA_LOC_TOPLEFT = 3,
    AVCHROMA_LOC_TOP = 4,
    AVCHROMA_LOC_BOTTOMLEFT = 5,
    AVCHROMA_LOC_BOTTOM = 6
};

std::string MatrixName(int cs, int cr, int width, int height) {
    if (cs == AVCOL_SPC_RGB)
        return "None";
    std::string range = cr == AVCOL_RANGE_JPEG ? "PC" : "TV";
    if (cs == AVCOL_SPC_UNSPECIFIED)
        cs = width > 1024 || height >= 600 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    switch (cs) {
        case AVCOL_SPC_BT709: return range + ".709";
        case AVCOL_SPC_FCC: return range + ".FCC";
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return range + ".601";
        case AVCOL_SPC_SMPTE240M: return range + ".240M";
        case AVCOL_SPC_BT2020_NCL: return range + ".2020";
        case AVCOL_SPC_BT2020_CL: return range + ".2020CL";
        case AVCOL_SPC_ICTCP: return range + ".ICTCP";
        default: return range + ".709";
    }
}

std::string PrimariesName(int primaries, int cs) {
    switch (primaries) {
        case AVCOL_PRI_BT709: return "BT.709";
        case AVCOL_PRI_BT470M: return "BT.470M";
        case AVCOL_PRI_BT470BG: return "BT.601-625";
        case AVCOL_PRI_SMPTE170M: return "BT.601-525";
        case AVCOL_PRI_SMPTE240M: return "SMPTE-240M";
        case AVCOL_PRI_FILM: return "Film C";
        case AVCOL_PRI_BT2020: return "BT.2020";
        case AVCOL_PRI_SMPTE431: return "DCI-P3";
        case AVCOL_PRI_SMPTE432: return "Display-P3";
        default: break;
    }
    switch (cs) {
        case AVCOL_SPC_BT709: return "BT.709";
        case AVCOL_SPC_BT470BG: return "BT.601-625";
        case AVCOL_SPC_SMPTE170M: return "BT.601-525";
        case AVCOL_SPC_SMPTE240M: return "SMPTE-240M";
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
        case AVCOL_SPC_ICTCP: return "BT.2020";
        default: return {};
    }
}

std::string TransferName(int transfer) {
    switch (transfer) {
        case AVCOL_TRC_GAMMA22: return "Gamma 2.2";
        case AVCOL_TRC_GAMMA28: return "Gamma 2.8";
        case AVCOL_TRC_LINEAR: return "Linear";
        case AVCOL_TRC_IEC61966_2_1: return "sRGB";
        case AVCOL_TRC_SMPTE2084: return "PQ";
        case AVCOL_TRC_ARIB_STD_B67: return "HLG";
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_SMPTE240M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12: return "BT.1886";
        default: return {};
    }
}

SourceFrameColorMetadata ColorMetadata(lsmas_video_frame_props_t const& props) {
    SourceFrameColorMetadata color;
    color.matrix = MatrixName(props.colorspace, props.color_range, props.width, props.height);
    color.primaries = PrimariesName(props.color_primaries, props.colorspace);
    color.transfer = TransferName(props.color_trc);
    color.range = props.color_range == AVCOL_RANGE_JPEG ? SourceFrameColorRange::Full : SourceFrameColorRange::Limited;
    if (props.colorspace == AVCOL_SPC_RGB)
        color.matrix = "None";
    return color;
}

SourceFrameChromaLocation ChromaLocation(int value) {
    switch (value) {
        case AVCHROMA_LOC_LEFT: return SourceFrameChromaLocation::Left;
        case AVCHROMA_LOC_CENTER: return SourceFrameChromaLocation::Center;
        case AVCHROMA_LOC_TOPLEFT: return SourceFrameChromaLocation::TopLeft;
        case AVCHROMA_LOC_TOP: return SourceFrameChromaLocation::TopCenter;
        case AVCHROMA_LOC_BOTTOMLEFT: return SourceFrameChromaLocation::BottomLeft;
        case AVCHROMA_LOC_BOTTOM: return SourceFrameChromaLocation::BottomCenter;
        default: return SourceFrameChromaLocation::Unknown;
    }
}

SourceFrameFormatInfo ConvertFormatInfo(lsmas_video_format_info_t const& info) {
    SourceFrameFormatInfo out;
    out.color_family = info.color_family == LSMAS_COLOR_FAMILY_RGB ? SourceFrameColorFamily::Rgb
        : info.color_family == LSMAS_COLOR_FAMILY_YCBCR ? SourceFrameColorFamily::YCbCr
        : SourceFrameColorFamily::Unknown;
    out.plane_count = info.plane_count;
    for (int i = 0; i < info.plane_count && i < 4; ++i) {
        auto const& src = info.planes[i];
        auto& dst = out.planes[static_cast<size_t>(i)];
        dst.width_divisor = std::max(1, src.width_divisor);
        dst.height_divisor = std::max(1, src.height_divisor);
        dst.components_per_sample = std::max(1, src.components_per_sample);
        dst.bytes_per_sample = std::max(1, src.bytes_per_sample);
        dst.bits_per_component = std::max(1, src.bits_per_component);
        for (int c = 0; c < 4; ++c)
            dst.component_shift[static_cast<size_t>(c)] = src.component_shift[c];
    }
    return out;
}

SourceFrameGeometry BuildGeometry(lsmas_video_frame_props_t const& props) {
    auto geometry = MakeDefaultSourceFrameGeometry(props.width, props.height);
    int visible_x = std::max(0, props.crop_left);
    int visible_y = std::max(0, props.crop_top);
    int visible_w = props.width - std::max(0, props.crop_left) - std::max(0, props.crop_right);
    int visible_h = props.height - std::max(0, props.crop_top) - std::max(0, props.crop_bottom);
    if (visible_w > 0 && visible_h > 0)
        geometry.visible_rect = { visible_x, visible_y, visible_w, visible_h };
    geometry.rotation = props.display_rotation_degrees;
    geometry.display_vflip = props.display_vflip != 0;
    if (props.sar_num > 0 && props.sar_den > 0)
        geometry.pixel_aspect_ratio = static_cast<double>(props.sar_num) / props.sar_den;
    return geometry;
}

class LsmasVideoProvider final : public VideoProvider {
    lsmas_handle_t *handle = nullptr;
    lsmas_video_info_t info = {};
    lsmas_video_frame_props_t first_props = {};
    SourceFrameColorMetadata color;
    SourceFrameGeometry geometry;
    SourceFrameFormatInfo native_format_info;
    int native_pix_fmt = -1;
    std::string native_format_name;
    std::string cache_filename_utf8;
    std::vector<int> keyframes;
    agi::vfr::Framerate timecodes;
    bool has_audio = false;
    SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;

public:
    LsmasVideoProvider(agi::fs::path const& filename, std::string const&, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
    ~LsmasVideoProvider() override {
        if (handle)
            lsmas::GetApi().av_close(handle);
    }

    void GetFrame(int n, VideoFrame &frame) override;
    bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override;
    void SetColorSpace(std::string const&) override { }

    int GetFrameCount() const override { return info.num_frames; }
    int GetWidth() const override { return GetSourceFrameDisplayOutputRect(geometry).width; }
    int GetHeight() const override { return GetSourceFrameDisplayOutputRect(geometry).height; }
    double GetDAR() const override { return GetSourceFrameDisplayAspectRatio(geometry); }
    agi::vfr::Framerate GetFPS() const override { return timecodes; }
    std::vector<int> GetKeyFrames() const override { return keyframes; }
    std::string GetColorSpace() const override { return color.matrix.empty() ? "None" : color.matrix; }
    SourceFrameColorMetadata GetColorMetadata() const override { return color; }
    SourceFrameColorMetadata GetRealColorMetadata() const override { return color; }
    SourceFrameGeometry GetFrameGeometry() const override { return output_mode == SourceFrameOutputMode::Native ? geometry : BakeSourceFrameGeometry(geometry); }
    SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, native_pix_fmt }; }
    std::string GetNativeFormatDescription() const override { return native_format_name; }
    std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override { return { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }; }
    bool SetOutputMode(SourceFrameOutputMode mode) override;
    std::string GetDecoderName() const override { return "LsmasNative"; }
    bool WantsCaching() const override { return false; }
    bool HasAudio() const override { return has_audio; }
};

class LsmasFrameOwner {
    lsmas_video_frame_t *frame = nullptr;
public:
    explicit LsmasFrameOwner(lsmas_video_frame_t *frame) : frame(frame) { }
    ~LsmasFrameOwner() { if (frame) lsmas::GetApi().video_release_frame(frame); }
    LsmasFrameOwner(LsmasFrameOwner const&) = delete;
    LsmasFrameOwner& operator=(LsmasFrameOwner const&) = delete;
};

std::vector<int> BuildKeyframes(lsmas_handle_t *handle) {
    auto const& api = lsmas::GetApi();
    lsmas_provider::ErrorString error;
    int32_t source_count = 0;
    if (api.video_get_source_frame_count(handle, &source_count, error.Out()) < 0 || source_count <= 0)
        source_count = 0;
    if (source_count <= 0)
        return {};

    std::vector<uint8_t> flags(static_cast<size_t>(source_count));
    error.Reset();
    int32_t got = api.video_get_source_keyframe_flags(handle, flags.data(), source_count, error.Out());
    if (got < 0)
        return {};

    std::vector<int> result;
    for (int32_t i = 0; i < got; ++i) {
        if (flags[static_cast<size_t>(i)])
            result.push_back(i);
    }
    return result;
}

agi::vfr::Framerate BuildTimecodes(lsmas_handle_t *handle, lsmas_video_info_t const& info) {
    auto const& api = lsmas::GetApi();
    if (info.num_frames < 2)
        return info.fps_num > 0 && info.fps_den > 0 ? agi::vfr::Framerate(info.fps_num, info.fps_den) : agi::vfr::Framerate(25.0);

    int32_t tb_num = 0;
    int32_t tb_den = 0;
    lsmas_provider::ErrorString error;
    if (api.video_get_time_base(handle, &tb_num, &tb_den, error.Out()) < 0 || tb_num <= 0 || tb_den <= 0)
        return info.fps_num > 0 && info.fps_den > 0 ? agi::vfr::Framerate(info.fps_num, info.fps_den) : agi::vfr::Framerate(25.0);

    std::vector<int64_t> pts(static_cast<size_t>(info.num_frames));
    error.Reset();
    int32_t got = api.video_get_pts_list(handle, pts.data(), info.num_frames, error.Out());
    if (got < info.num_frames)
        return info.fps_num > 0 && info.fps_den > 0 ? agi::vfr::Framerate(info.fps_num, info.fps_den) : agi::vfr::Framerate(25.0);

    std::vector<int> ms;
    ms.reserve(pts.size());
    for (auto value : pts) {
        long double t = static_cast<long double>(value) * static_cast<long double>(tb_num) * 1000.0L / static_cast<long double>(tb_den);
        ms.push_back(static_cast<int>(t >= 0 ? t + 0.5L : t - 0.5L));
    }
    return agi::vfr::Framerate(ms);
}

LsmasVideoProvider::LsmasVideoProvider(agi::fs::path const& filename, std::string const&, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    auto const& api = lsmas::GetApi();
    auto const filename_utf8 = agi::fs::PathToString(filename);

    int stream_index = lsmas_provider::SelectTrack(filename, lsmas_provider::TrackType::Video, choice_sink);
    if (stream_index < 0)
        throw VideoNotSupported("no video tracks found");

    auto cache_name = lsmas_provider::GetIndexCacheFilename(filename);
    cache_filename_utf8 = agi::fs::PathToString(cache_name);

    auto video_options = lsmas_provider::MakeVideoOpenOptions(stream_index);
    video_options.cachefile = cache_filename_utf8.c_str();

    auto audio_options = lsmas_provider::MakeAudioOpenOptions(-1, OPT_GET("Provider/Audio/LsmasNative/Downmix")->GetBool());
    audio_options.cachefile = cache_filename_utf8.c_str();

    lsmas_provider::ErrorString error;
    if (br) {
        br->Run([&](agi::ProgressSink *ps) {
            ps->SetTitle("Indexing");
            ps->SetMessage("Reading audio, timecodes and frame data");
            handle = api.av_open_with_progress_utf8(filename_utf8.c_str(), &video_options, &audio_options, lsmas_provider::ProgressCallback, ps, error.Out());
        });
    }
    else {
        handle = api.av_open_with_progress_utf8(filename_utf8.c_str(), &video_options, &audio_options, nullptr, nullptr, error.Out());
    }
    if (!handle)
        throw VideoOpenError(error.Message("failed to open video"));
    auto close_handle_on_error = agi::make_scope_exit([&] {
        if (handle) {
            api.av_close(handle);
            handle = nullptr;
        }
    });
    agi::fs::Touch(cache_name);
    lsmas_provider::CleanIndexCache();

    lsmas_audio_info_t audio_info = {};
    error.Reset();
    has_audio = api.audio_get_info(handle, &audio_info, error.Out()) >= 0;

    error.Reset();
    if (api.video_get_info(handle, &info, error.Out()) < 0 || info.width <= 0 || info.height <= 0 || info.num_frames <= 0)
        throw VideoOpenError(error.Message("failed to query video info"));

    lsmas_video_frame_t *first = nullptr;
    error.Reset();
    if (api.video_acquire_avframe(handle, 0, &first, error.Out()) < 0)
        throw VideoOpenError(error.Message("failed to decode first frame"));
    std::unique_ptr<LsmasFrameOwner> first_owner = std::make_unique<LsmasFrameOwner>(first);

    if (api.video_frame_get_props(first, &first_props, error.Out()) < 0)
        throw VideoOpenError(error.Message("failed to query first frame metadata"));
    lsmas_video_format_info_t fmt = {};
    if (api.video_frame_get_format_info(first, &fmt, error.Out()) < 0)
        throw VideoOpenError(error.Message("failed to query first frame format"));

    color = ColorMetadata(first_props);
    geometry = BuildGeometry(first_props);
    native_format_info = ConvertFormatInfo(fmt);
    native_pix_fmt = first_props.pix_fmt;
    if (auto *name = api.video_frame_get_pix_fmt_name(first))
        native_format_name = std::string(name) + " (ffmpeg:" + std::to_string(native_pix_fmt) + ")";
    else
        native_format_name = "ffmpeg:" + std::to_string(native_pix_fmt);

    keyframes = BuildKeyframes(handle);
    timecodes = BuildTimecodes(handle, info);
    close_handle_on_error.release();
}

bool LsmasVideoProvider::SetOutputMode(SourceFrameOutputMode mode) {
    if (mode != SourceFrameOutputMode::Native && mode != SourceFrameOutputMode::Bgra8)
        return false;
    output_mode = mode;
    return true;
}

void LsmasVideoProvider::GetFrame(int n, VideoFrame &frame) {
    auto const& api = lsmas::GetApi();
    n = std::clamp(n, 0, info.num_frames - 1);

    lsmas_provider::ErrorString error;
    lsmas_video_frame_buffer_layout_t layout = {};
    int64_t required = api.video_get_frame(handle, n, LSMAS_VIDEO_FRAME_OUTPUT_BGRA, nullptr, 0, &layout, error.Out());
    if (required < 0)
        throw VideoDecodeError(error.Message("failed to decode BGRA frame"));

    if (layout.width <= 0 || layout.height <= 0 || layout.plane_count != 1 || layout.plane_stride[0] <= 0 || required <= 0)
        throw VideoDecodeError("invalid BGRA frame layout returned by LsmasNative");
    if (static_cast<uint64_t>(required) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw VideoDecodeError("BGRA frame is too large");

    auto const pitch = static_cast<size_t>(layout.plane_stride[0]);
    auto const height = static_cast<size_t>(layout.height);
    if (height && pitch > std::numeric_limits<size_t>::max() / height)
        throw VideoDecodeError("BGRA frame dimensions are too large");
    if (static_cast<size_t>(required) < pitch * height)
        throw VideoDecodeError("invalid BGRA frame buffer size returned by LsmasNative");

    frame.width = static_cast<size_t>(layout.width);
    frame.height = height;
    frame.pitch = pitch;
    frame.flipped = false;
    frame.data.resize(static_cast<size_t>(required));

    error.Reset();
    required = api.video_get_frame(handle, n, LSMAS_VIDEO_FRAME_OUTPUT_BGRA, frame.data.data(), layout.plane_stride[0], &layout, error.Out());
    if (required < 0)
        throw VideoDecodeError(error.Message("failed to decode BGRA frame"));
    if (static_cast<uint64_t>(required) > static_cast<uint64_t>(frame.data.size()))
        throw VideoDecodeError("BGRA frame grew during decode");
}

bool LsmasVideoProvider::GetNativeFrame(int n, SourceFrame& out, std::shared_ptr<void>& owner) {
    auto const& api = lsmas::GetApi();
    n = std::clamp(n, 0, info.num_frames - 1);

    lsmas_provider::ErrorString error;
    lsmas_video_frame_t *native = nullptr;
    if (api.video_acquire_avframe(handle, n, &native, error.Out()) < 0)
        throw VideoDecodeError(error.Message("failed to acquire native frame"));

    auto native_owner = std::make_shared<LsmasFrameOwner>(native);
    lsmas_video_frame_props_t props = {};
    if (api.video_frame_get_props(native, &props, error.Out()) < 0)
        throw VideoDecodeError(error.Message("failed to query native frame metadata"));
    lsmas_video_format_info_t fmt = {};
    if (api.video_frame_get_format_info(native, &fmt, error.Out()) < 0)
        throw VideoDecodeError(error.Message("failed to query native frame format"));

    auto format_info = ConvertFormatInfo(fmt);
    out = {};
    out.output_mode = SourceFrameOutputMode::Native;
    out.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, props.pix_fmt };
    out.native_payload_kind = SourceFrameNativePayloadKind::FFmpegAVFrame;
    out.native_payload = api.video_frame_get_avframe(native);
    out.format_info = format_info;
    out.width = props.width;
    out.height = props.height;
    out.flipped = false;
    out.plane_count = format_info.plane_count;
    out.geometry = BuildGeometry(props);
    out.color = ColorMetadata(props);
    out.chroma_location = ChromaLocation(props.chroma_location);

    for (int i = 0; i < out.plane_count; ++i) {
        const uint8_t *data = nullptr;
        int32_t stride = 0;
        int32_t width = 0;
        int32_t height = 0;
        if (api.video_frame_get_plane(native, i, &data, &stride, &width, &height, error.Out()) < 0)
            throw VideoDecodeError(error.Message("failed to query native frame plane"));
        out.planes[static_cast<size_t>(i)] = { data, stride, width, height };
    }

    owner = native_owner;
    return true;
}
}

std::unique_ptr<VideoProvider> CreateLsmasNativeVideoProvider(agi::fs::path const& path, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    return agi::make_unique<LsmasVideoProvider>(path, colormatrix, br, std::move(choice_sink));
}

#endif
