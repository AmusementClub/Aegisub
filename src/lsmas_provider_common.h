#pragma once

#include "lsmas_native_api.h"

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agi {
class BackgroundRunner;
class SingleChoiceInteractionSink;
}

namespace lsmas_provider {

enum class TrackType {
    Video,
    Audio
};

struct TrackRational {
    int64_t numerator = 0;
    int64_t denominator = 0;
};

struct TrackChoice {
    int stream_index = -1;
    std::string codec_name;
    std::string language;
    std::string title;
    int channels = 0;
    int width = 0;
    int height = 0;
    TrackRational frame_rate;
    std::string display_name;
};

struct ErrorString {
    char *value = nullptr;

    ErrorString() = default;
    ErrorString(ErrorString const&) = delete;
    ErrorString& operator=(ErrorString const&) = delete;
    ~ErrorString();

    char **Out() { return &value; }
    void Reset();
    std::string Message(std::string const& fallback = {}) const;
};

std::vector<TrackChoice> ParseTrackChoicesJson(std::string_view json_text, TrackType type);
std::vector<TrackChoice> ProbeTracks(agi::fs::path const& filename, TrackType type);
int SelectTrack(agi::fs::path const& filename,
                TrackType type,
                std::shared_ptr<agi::SingleChoiceInteractionSink> const& choice_sink);

agi::fs::path GetIndexCacheFilename(agi::fs::path const& filename);
void CleanIndexCache();

lsmas_video_open_options_t MakeVideoOpenOptions(int stream_index);
lsmas_audio_open_options_t MakeAudioOpenOptions(int stream_index, bool downmix);

int ProgressCallback(void *userdata, const char *message_utf8, int32_t percent);

// Upper bound on native read attempts used to ride out a transient short read
// before the remainder of a request is treated as missing.
constexpr int kAudioShortReadMaxAttempts = 3;

// Reads `count` sample frames through `read_frames(dst, start, count)`, which
// returns the number of frames written or a negative value on error. The
// native reader can transiently return fewer frames than requested for a
// mid-stream range; the missing sub-range is re-read until the request
// completes, stops making progress, or the attempt limit is reached. Returns
// the number of frames written (possibly less than count) or the negative
// error value unchanged.
template <typename ReadFrames>
int64_t ReadAudioFramesWithRetry(
	void *dst,
	int64_t start,
	int64_t count,
	int64_t frame_bytes,
	ReadFrames read_frames) {
	auto *cursor = static_cast<unsigned char *>(dst);
	int64_t frames = 0;
	for (int attempts = 0; frames < count && attempts < kAudioShortReadMaxAttempts; ++attempts) {
		int64_t const got = read_frames(
			cursor + frames * frame_bytes,
			start + frames,
			count - frames);
		if (got < 0)
			return got;
		frames += got;
		if (frames < count && attempts + 1 < kAudioShortReadMaxAttempts)
			std::this_thread::yield();
	}
	return frames;
}
}
