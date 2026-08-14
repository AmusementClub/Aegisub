#include <main.h>

#include "../../src/lsmas_provider_common.h"

using lsmas_provider::ParseTrackChoicesJson;
using lsmas_provider::TrackType;

TEST(lsmas_provider_common, video_labels_include_dimensions_frame_rate_language_and_title) {
    auto const tracks = ParseTrackChoicesJson(R"json({
        "streams": [
            {
                "index": 0,
                "type": "video",
                "codec": "h264",
                "default": true,
                "forced": true,
                "language": "jpn",
                "title": "Main angle",
                "width": 1920,
                "height": 1080,
                "avg_frame_rate": {"num": 24000, "den": 1001}
            }
        ]
    })json", TrackType::Video);

    ASSERT_EQ(1U, tracks.size());
    EXPECT_EQ(0, tracks[0].stream_index);
    EXPECT_EQ(1920, tracks[0].width);
    EXPECT_EQ(1080, tracks[0].height);
    EXPECT_EQ(24000, tracks[0].frame_rate.numerator);
    EXPECT_EQ(1001, tracks[0].frame_rate.denominator);
    EXPECT_EQ("Track 00: h264, 1920x1080, 24000/1001 fps, jpn: Main angle", tracks[0].display_name);
}

TEST(lsmas_provider_common, audio_labels_include_channels_language_and_title) {
    auto const tracks = ParseTrackChoicesJson(R"json({
        "streams": [
            {"index": 0, "type": "video", "codec": "hevc"},
            {
                "index": 2,
                "type": "audio",
                "codec": "aac",
                "language": "eng",
                "title": "Commentary",
                "channels": 6,
                "sample_rate": 48000
            }
        ]
    })json", TrackType::Audio);

    ASSERT_EQ(1U, tracks.size());
    EXPECT_EQ(2, tracks[0].stream_index);
    EXPECT_EQ(6, tracks[0].channels);
    EXPECT_EQ("Track 02: aac, 6 ch, eng: Commentary", tracks[0].display_name);
}

TEST(lsmas_provider_common, old_probe_json_keeps_compact_fallback_labels) {
    auto const tracks = ParseTrackChoicesJson(R"json({
        "streams": [
            {"index": -1, "type": "video", "codec": "invalid"},
            {"index": 3, "type": "video", "codec": "hevc"},
            {
                "index": 4,
                "type": "video",
                "codec": "av1",
                "width": 3840,
                "height": 0,
                "avg_frame_rate": {"num": 0, "den": 1}
            }
        ]
    })json", TrackType::Video);

    ASSERT_EQ(2U, tracks.size());
    EXPECT_EQ("Track 03: hevc", tracks[0].display_name);
    EXPECT_EQ("Track 04: av1", tracks[1].display_name);
}

TEST(lsmas_provider_common, frame_rates_keep_exact_rational_values) {
    auto const tracks = ParseTrackChoicesJson(R"json({
        "streams": [
            {
                "index": 1,
                "type": "video",
                "codec": "mpeg2video",
                "width": 720,
                "height": 576,
                "avg_frame_rate": {"num": 25, "den": 1}
            }
        ]
    })json", TrackType::Video);

    ASSERT_EQ(1U, tracks.size());
    EXPECT_EQ("Track 01: mpeg2video, 720x576, 25/1 fps", tracks[0].display_name);
}
