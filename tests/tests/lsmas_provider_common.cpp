#include <main.h>

#include "../../src/lsmas_provider_common.h"

#include <cstdint>
#include <vector>

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

TEST(lsmas_provider_common, audio_retry_recovers_transient_short_read) {
    std::vector<std::int64_t> read_starts;
    int calls = 0;
    std::int16_t buffer[4] = {};

    auto const frames = lsmas_provider::ReadAudioFramesWithRetry(
        buffer, 10, 4, sizeof(std::int16_t),
        [&](void *dst, std::int64_t start, std::int64_t count) {
            ++calls;
            read_starts.push_back(start);
            auto const deliver = calls == 1 ? 1 : count;
            auto *out = static_cast<std::int16_t *>(dst);
            for (std::int64_t i = 0; i < deliver; ++i)
                out[i] = 42;
            return deliver;
        });

    EXPECT_EQ(4, frames);
    EXPECT_EQ(2, calls);
    ASSERT_EQ(2u, read_starts.size());
    EXPECT_EQ(10, read_starts[0]);
    EXPECT_EQ(11, read_starts[1]);
    EXPECT_EQ(42, buffer[3]);
}

TEST(lsmas_provider_common, audio_retry_resumes_interleaved_frames_at_frame_boundaries) {
    int calls = 0;
    // Stereo int16: one frame is 4 bytes, matching LsmasAudioProvider.
    std::int16_t buffer[2 * 3] = {};

    auto const frames = lsmas_provider::ReadAudioFramesWithRetry(
        buffer, 0, 3, 2 * static_cast<std::int64_t>(sizeof(std::int16_t)),
        [&](void *dst, std::int64_t, std::int64_t count) {
            ++calls;
            auto const deliver = calls == 1 ? 2 : count;
            auto *out = static_cast<std::int16_t *>(dst);
            for (std::int64_t i = 0; i < deliver; ++i) {
                out[i * 2 + 0] = 100;
                out[i * 2 + 1] = -100;
            }
            return deliver;
        });

    ASSERT_EQ(3, frames);
    ASSERT_EQ(2, calls);
    for (std::size_t frame = 0; frame < 3; ++frame) {
        EXPECT_EQ(100, buffer[frame * 2 + 0]);
        EXPECT_EQ(-100, buffer[frame * 2 + 1]);
    }
}

TEST(lsmas_provider_common, audio_retry_bounded_when_read_makes_no_progress) {
    int calls = 0;
    std::int16_t buffer[2] = {};

    auto const frames = lsmas_provider::ReadAudioFramesWithRetry(
        buffer, 0, 2, sizeof(std::int16_t),
        [&](void *, std::int64_t, std::int64_t) {
            ++calls;
            return 0;
        });

    EXPECT_EQ(0, frames);
    EXPECT_EQ(lsmas_provider::kAudioShortReadMaxAttempts, calls);
}

TEST(lsmas_provider_common, audio_retry_propagates_error_immediately) {
    int calls = 0;
    std::int16_t buffer[2] = {};

    auto const frames = lsmas_provider::ReadAudioFramesWithRetry(
        buffer, 0, 2, sizeof(std::int16_t),
        [&](void *, std::int64_t, std::int64_t) {
            ++calls;
            return -5;
        });

    EXPECT_EQ(-5, frames);
    EXPECT_EQ(1, calls);
}

TEST(lsmas_provider_common, audio_retry_reports_persistent_shortfall) {
    int calls = 0;
    std::int16_t buffer[5] = {};

    auto const frames = lsmas_provider::ReadAudioFramesWithRetry(
        buffer, 0, 5, sizeof(std::int16_t),
        [&](void *dst, std::int64_t, std::int64_t) {
            ++calls;
            *static_cast<std::int16_t *>(dst) = 7;
            return 1;
        });

    EXPECT_EQ(lsmas_provider::kAudioShortReadMaxAttempts, frames);
    EXPECT_EQ(lsmas_provider::kAudioShortReadMaxAttempts, calls);
}
