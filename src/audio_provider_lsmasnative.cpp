#ifdef WITH_LSMASNATIVE

#include <libaegisub/audio/provider.h>

#include "lsmas_native_api.h"
#include "lsmas_provider_common.h"
#include "options.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <memory>

namespace {
class LsmasAudioProvider final : public agi::AudioProvider {
    lsmas_handle_t *handle = nullptr;

    void FillBuffer(void *buf, int64_t start, int64_t count) const override {
        lsmas_provider::ErrorString error;
        auto got = lsmas::GetApi().audio_get_samples(handle, buf, start, count, error.Out());
        if (got < 0)
            throw agi::AudioDecodeError(error.Message("failed to decode audio samples"));
        if (got < count) {
            auto *bytes = static_cast<unsigned char *>(buf);
            auto offset = got * channels * bytes_per_sample;
            auto remaining = (count - got) * channels * bytes_per_sample;
            std::fill(bytes + offset, bytes + offset + remaining, 0);
        }
    }

public:
    LsmasAudioProvider(agi::fs::path const& filename, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
    ~LsmasAudioProvider() override {
        if (handle)
            lsmas::GetApi().audio_close(handle);
    }

    bool NeedsCache() const override { return true; }
    agi::AudioProviderMemoryStats GetMemoryStats() const override { return BuildMemoryStats("LsmasNative"); }
};

LsmasAudioProvider::LsmasAudioProvider(agi::fs::path const& filename, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    auto const& api = lsmas::GetApi();
    auto const filename_utf8 = agi::fs::PathToString(filename);

    int stream_index = lsmas_provider::SelectTrack(filename, lsmas_provider::TrackType::Audio, choice_sink);
    if (stream_index < 0)
        throw agi::AudioDataNotFound("no audio tracks found");

    lsmas_audio_open_options_t options = {};
    options.stream_index = stream_index;
    options.threads = 0;
    options.av_sync = 1;
    options.cache_index = 1;
    options.sample_format = LSMAS_AUDIO_S16;
    if (OPT_GET("Provider/Audio/LsmasNative/Downmix")->GetBool())
        options.channel_layout = 0x4; // AV_CH_FRONT_CENTER

    lsmas_provider::ErrorString error;
    if (br) {
        br->Run([&](agi::ProgressSink *ps) {
            ps->SetTitle("Indexing");
            ps->SetMessage("Reading audio sample data");
            handle = api.audio_open_with_progress_utf8(filename_utf8.c_str(), &options, lsmas_provider::ProgressCallback, ps, error.Out());
        });
    }
    else {
        handle = api.audio_open_with_progress_utf8(filename_utf8.c_str(), &options, nullptr, nullptr, error.Out());
    }
    if (!handle)
        throw agi::AudioProviderError(error.Message("failed to open audio"));
    auto close_handle_on_error = agi::make_scope_exit([&] {
        if (handle) {
            api.audio_close(handle);
            handle = nullptr;
        }
    });

    lsmas_audio_info_t info = {};
    error.Reset();
    if (api.audio_get_info(handle, &info, error.Out()) < 0)
        throw agi::AudioProviderError(error.Message("failed to query audio info"));

    channels = info.channels;
    sample_rate = info.sample_rate;
    bytes_per_sample = info.bytes_per_sample > 0 ? info.bytes_per_sample : 2;
    float_samples = false;
    num_samples = info.total_samples;
    decoded_samples = info.decoded_samples;
    if (channels <= 0 || sample_rate <= 0 || num_samples <= 0)
        throw agi::AudioProviderError("invalid audio properties returned by LsmasNative");
    close_handle_on_error.release();
}
}

std::unique_ptr<agi::AudioProvider> CreateLsmasNativeAudioProvider(agi::fs::path const& file, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    return agi::make_unique<LsmasAudioProvider>(file, br, std::move(choice_sink));
}

#endif
