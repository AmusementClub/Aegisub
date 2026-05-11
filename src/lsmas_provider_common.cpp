#include "lsmas_provider_common.h"

#include "track_choice.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/cajun/reader.h>

#include <sstream>

namespace lsmas_provider {

ErrorString::~ErrorString() {
    if (value)
        lsmas::GetApi().free(value);
}

void ErrorString::Reset() {
    if (value)
        lsmas::GetApi().free(value);
    value = nullptr;
}

std::string ErrorString::Message(std::string const& fallback) const {
    if (value && *value)
        return value;
    return fallback;
}

namespace {
std::string TrackTypeName(TrackType type) {
    return type == TrackType::Video ? "video" : "audio";
}

std::string GetString(json::Object const& object, char const *key) {
    auto it = object.find(key);
    if (it == object.end())
        return {};
    try {
        return static_cast<json::String const&>(it->second);
    }
    catch (...) {
        return {};
    }
}

int64_t GetInteger(json::Object const& object, char const *key, int64_t fallback = 0) {
    auto it = object.find(key);
    if (it == object.end())
        return fallback;
    try {
        return static_cast<json::Integer const&>(it->second);
    }
    catch (...) {
        return fallback;
    }
}

std::string BuildTrackLabel(json::Object const& stream) {
    auto index = GetInteger(stream, "index", -1);
    auto codec = GetString(stream, "codec");
    if (codec.empty())
        codec = "unknown";

    std::string label = "Track " + (index < 10 ? std::string("0") : std::string()) + std::to_string(index) + ": " + codec;
    auto language = GetString(stream, "language");
    if (!language.empty()) {
        label += ", ";
        label += language;
    }

    return label;
}
}

std::vector<TrackChoice> ProbeTracks(agi::fs::path const& filename, TrackType type) {
    auto const& api = lsmas::GetApi();
    auto const filename_utf8 = agi::fs::PathToString(filename);

    ErrorString error;
    char *json_text = api.probe_streams_json_utf8(filename_utf8.c_str(), error.Out());
    if (!json_text)
        throw agi::EnvironmentError(error.Message("failed to probe media streams"));
    std::unique_ptr<char, decltype(api.free)> json_holder(json_text, api.free);

    std::istringstream stream(json_text);
    json::UnknownElement root;
    json::Reader::Read(root, stream);
    auto const& root_object = static_cast<json::Object const&>(root);
    auto streams_it = root_object.find("streams");
    if (streams_it == root_object.end())
        return {};

    std::vector<TrackChoice> tracks;
    for (auto const& item : static_cast<json::Array const&>(streams_it->second)) {
        auto const& object = static_cast<json::Object const&>(item);
        if (GetString(object, "type") != TrackTypeName(type))
            continue;

        TrackChoice choice;
        choice.stream_index = static_cast<int>(GetInteger(object, "index", -1));
        choice.codec_name = GetString(object, "codec");
        choice.display_name = BuildTrackLabel(object);
        tracks.push_back(std::move(choice));
    }
    return tracks;
}

int SelectTrack(agi::fs::path const& filename,
                TrackType type,
                std::shared_ptr<agi::SingleChoiceInteractionSink> const& choice_sink) {
    auto tracks = ProbeTracks(filename, type);
    if (tracks.empty())
        return -1;
    if (tracks.size() == 1)
        return tracks.front().stream_index;
    if (!choice_sink)
        return tracks.front().stream_index;

    std::vector<std::string> choices;
    choices.reserve(tracks.size());
    for (auto const& track : tracks)
        choices.push_back(track.display_name);

    auto choice = choice_sink->RequestSingleChoice(aegisub::track_choice::BuildRequest(
        type == TrackType::Video ? aegisub::track_choice::DialogKind::Video : aegisub::track_choice::DialogKind::Audio,
        choices));
    auto resolved = aegisub::track_choice::ResolveSelection(tracks.size(), choice);
    if (!resolved)
        throw agi::UserCancelException(type == TrackType::Video ? "video loading canceled by user" : "audio loading canceled by user");
    return tracks[*resolved].stream_index;
}

int ProgressCallback(void *userdata, const char *message_utf8, int32_t percent) {
    auto *ps = static_cast<agi::ProgressSink *>(userdata);
    if (!ps)
        return 0;
    if (message_utf8 && *message_utf8)
        ps->SetMessage(message_utf8);
    ps->SetProgress(percent, 100);
    return ps->IsCancelled() ? 1 : 0;
}

}
