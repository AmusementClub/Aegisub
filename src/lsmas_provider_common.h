#pragma once

#include "lsmas_native_api.h"

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <string>
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

struct TrackChoice {
    int stream_index = -1;
    std::string codec_name;
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

std::vector<TrackChoice> ProbeTracks(agi::fs::path const& filename, TrackType type);
int SelectTrack(agi::fs::path const& filename,
                TrackType type,
                std::shared_ptr<agi::SingleChoiceInteractionSink> const& choice_sink);

int ProgressCallback(void *userdata, const char *message_utf8, int32_t percent);

}
