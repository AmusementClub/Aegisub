#include <main.h>

#include "../../src/lsmas_provider_common.h"
#include "../../src/audio_display_source.h"
#include "../../src/audio_provider_factory.h"
#include "../../src/options.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <thread>
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

TEST(lsmas_provider_common, audio_retry_throws_when_read_makes_no_progress) {
	int calls = 0;
	std::int16_t buffer[2] = {17, 23};

	EXPECT_THROW(lsmas_provider::ReadAudioFramesWithRetry(
					 buffer, 0, 2, sizeof(std::int16_t),
					 [&](void *, std::int64_t, std::int64_t) {
						 ++calls;
						 return 0;
					 }),
				 agi::AudioDecodeError);

	EXPECT_EQ(lsmas_provider::kAudioShortReadMaxAttempts, calls);
	EXPECT_EQ(17, buffer[0]);
	EXPECT_EQ(23, buffer[1]);
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

TEST(lsmas_provider_common, audio_retry_throws_on_persistent_shortfall_without_zero_filling) {
	int calls = 0;
	std::int16_t buffer[5] = {17, 17, 17, 17, 17};

	EXPECT_THROW(lsmas_provider::ReadAudioFramesWithRetry(
					 buffer, 0, 5, sizeof(std::int16_t),
					 [&](void *dst, std::int64_t, std::int64_t) {
						 ++calls;
						 *static_cast<std::int16_t *>(dst) = 7;
						 return 1;
					 }),
				 agi::AudioDecodeError);

	EXPECT_EQ(lsmas_provider::kAudioShortReadMaxAttempts, calls);
	EXPECT_EQ(7, buffer[0]);
	EXPECT_EQ(7, buffer[2]);
	EXPECT_EQ(17, buffer[3]);
	EXPECT_EQ(17, buffer[4]);
}

TEST(lsmas_provider_common, audio_retry_recovers_on_last_attempt_after_no_progress) {
	int calls = 0;
	std::int16_t buffer[2] = {17, 23};

	auto const frames = lsmas_provider::ReadAudioFramesWithRetry(
		buffer, 10, 2, sizeof(std::int16_t),
		[&](void *dst, std::int64_t start, std::int64_t count) -> std::int64_t {
			++calls;
			EXPECT_EQ(10, start);
			EXPECT_EQ(2, count);
			if (calls < lsmas_provider::kAudioShortReadMaxAttempts)
				return 0;
			auto *out = static_cast<std::int16_t *>(dst);
			out[0] = 42;
			out[1] = -42;
			return count;
		});

	EXPECT_EQ(2, frames);
	EXPECT_EQ(lsmas_provider::kAudioShortReadMaxAttempts, calls);
	EXPECT_EQ(42, buffer[0]);
	EXPECT_EQ(-42, buffer[1]);
}

namespace {
struct CompressedAudioCacheCase {
	char const *name;
	char const *filename;
	int64_t frames;
	int sample_rate;
	bool disk_cache;
	bool exact_seek_samples;
};

class LsmasCompressedAudioCacheTest : public ::testing::TestWithParam<CompressedAudioCacheCase> {};
}

TEST_P(LsmasCompressedAudioCacheTest, complete_and_random_reads_match_uncached_audio) {
	if (!lsmas::IsAvailable())
		GTEST_SKIP() << "LsmasNative runtime unavailable: " << lsmas::GetLoadError();

	auto const& scenario = GetParam();
	auto root = agi::fs::UniquePath(std::filesystem::temp_directory_path() / "aegisub-lsmas-compressed-%%%%%%%%");
	agi::fs::CreateDirectory(root);
	agi::Path paths;
	paths.SetToken("?local", root);
	auto *previous_path = config::path;
	auto *previous_options = config::opt;
	config::path = &paths;
	config::opt = nullptr;
	auto restore_config = agi::make_scope_exit([&] {
		config::path = previous_path;
		config::opt = previous_options;
		std::error_code error;
		std::filesystem::remove_all(root, error);
	});
	auto const fixture = std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / "tests/fixtures/audio" / scenario.filename;
	auto source = GetAudioProviderWithPreferred(fixture, "LsmasNative", nullptr, nullptr);
	ASSERT_EQ("LsmasNative", source->GetMemoryStats().provider_name);
	ASSERT_EQ(2, source->GetChannels());
	ASSERT_EQ(2, source->GetBytesPerSample());
	ASSERT_FALSE(source->AreSamplesFloat());
	ASSERT_EQ(scenario.sample_rate, source->GetSampleRate());
	ASSERT_EQ(scenario.frames, source->GetNumSamples());
	ASSERT_EQ(scenario.frames, source->GetDecodedSamples());
	std::vector<int16_t> reference(static_cast<size_t>(scenario.frames) * 2, -32768);
	ASSERT_NO_THROW(source->GetAudioChecked(reference.data(), 0, scenario.frames));
	// A generated low-amplitude sine contains both signs and never reaches
	// full scale. Preserve the sentinel check so a short write cannot pass.
	EXPECT_EQ(reference.end(), std::ranges::find(reference, int16_t{-32768}));
	EXPECT_TRUE(std::ranges::any_of(reference, [](int16_t sample) { return sample > 0; }));
	EXPECT_TRUE(std::ranges::any_of(reference, [](int16_t sample) { return sample < 0; }));

	auto cached_source = GetAudioProviderWithPreferred(fixture, "LsmasNative", nullptr, nullptr);
	ASSERT_EQ("LsmasNative", cached_source->GetMemoryStats().provider_name);
	auto cache = scenario.disk_cache
					 ? agi::CreateHDAudioProvider(std::move(cached_source), root)
					 : agi::CreateRAMAudioProvider(std::move(cached_source));
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (cache->GetDecodedSamples() < cache->GetNumSamples() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_EQ(scenario.frames, cache->GetDecodedSamples());
	std::vector<int16_t> complete(reference.size(), -32768);
	ASSERT_NO_THROW(cache->GetAudioChecked(complete.data(), 0, scenario.frames));
	EXPECT_EQ(reference, complete);

	std::vector<std::pair<int64_t, int64_t>> ranges{
		{scenario.frames - 1, 1}, {0, 128}, {scenario.frames - 127, 256}, {-64, 128}, {scenario.frames / 2, 1024}, {1, 129}, {scenario.frames, 64}, {-128, 64}};
	std::mt19937_64 random(0x5A17);
	for (int index = 0; index < 16; ++index)
		ranges.emplace_back(static_cast<int64_t>(random() % scenario.frames), 1 + random() % 2048);
	for (auto const [start, count] : ranges) {
		SCOPED_TRACE(start);
		SCOPED_TRACE(count);
		std::vector<int16_t> expected(static_cast<size_t>(count) * 2, 0);
		for (int64_t frame = 0; frame < count; ++frame) {
			auto const source_frame = start + frame;
			if (source_frame >= 0 && source_frame < scenario.frames)
				std::ranges::copy_n(reference.data() + source_frame * 2, 2, expected.data() + frame * 2);
		}
		std::vector<int16_t> uncached(expected.size(), -32768);
		std::vector<int16_t> cached(expected.size(), -32768);
		ASSERT_NO_THROW(source->GetAudioChecked(uncached.data(), start, count));
		ASSERT_NO_THROW(cache->GetAudioChecked(cached.data(), start, count));
		if (scenario.exact_seek_samples)
			EXPECT_EQ(expected, uncached);
		else {
			// AAC noise substitution and Opus seek/preroll can differ from a
			// full sequential decode. Both caches must preserve that PCM exactly.
			EXPECT_EQ(uncached.end(), std::ranges::find(uncached, int16_t{-32768}));
			for (int64_t frame = 0; frame < count; ++frame) {
				if (start + frame < 0 || start + frame >= scenario.frames) {
					EXPECT_EQ(0, uncached[frame * 2]);
					EXPECT_EQ(0, uncached[frame * 2 + 1]);
				}
			}
			auto const first_frame = std::clamp<int64_t>(start, 0, scenario.frames);
			auto const last_frame = std::clamp<int64_t>(start + count, 0, scenario.frames);
			auto const two_periods = (static_cast<int64_t>(scenario.sample_rate) * 2 + 996) / 997;
			if (last_frame - first_frame >= two_periods) {
				auto const actual_first = uncached.begin() + (first_frame - start) * 2;
				auto const actual_last = uncached.begin() + (last_frame - start) * 2;
				auto const expected_first = reference.begin() + first_frame * 2;
				auto const expected_last = reference.begin() + last_frame * 2;
				// Preserve genuine codec padding while rejecting lost tone data.
				if (std::ranges::any_of(expected_first, expected_last, [](int16_t sample) { return sample > 0; }))
					EXPECT_TRUE(std::ranges::any_of(actual_first, actual_last, [](int16_t sample) { return sample > 0; }));
				if (std::ranges::any_of(expected_first, expected_last, [](int16_t sample) { return sample < 0; }))
					EXPECT_TRUE(std::ranges::any_of(actual_first, actual_last, [](int16_t sample) { return sample < 0; }));
			}
		}
		EXPECT_EQ(expected, cached);
	}
}

INSTANTIATE_TEST_SUITE_P(
	CompressedAudio,
	LsmasCompressedAudioCacheTest,
	::testing::Values(
		CompressedAudioCacheCase{"Mp3Ram", "synthetic-tone.mp3", 44673, 44100, false, true},
		CompressedAudioCacheCase{"Mp3Disk", "synthetic-tone.mp3", 44673, 44100, true, true},
		CompressedAudioCacheCase{"AacRam", "synthetic-tone.m4a", 44673, 44100, false, false},
		CompressedAudioCacheCase{"AacDisk", "synthetic-tone.m4a", 44673, 44100, true, false},
		CompressedAudioCacheCase{"OpusRam", "synthetic-tone.opus", 48624, 48000, false, false},
		CompressedAudioCacheCase{"OpusDisk", "synthetic-tone.opus", 48624, 48000, true, false}),
	[](auto const& info) { return info.param.name; });
