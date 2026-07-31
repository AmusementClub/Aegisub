#pragma once

#include <cstdint>
#include <memory>

namespace agi { class AudioProvider; }

class AudioDisplaySource {
public:
	virtual ~AudioDisplaySource() = default;

	virtual int64_t GetNumSamples() const = 0;
	virtual int GetChannels() const = 0;
	virtual int GetSampleRate() const = 0;
	virtual void GetFloatAudio(float *buf, int64_t start, int64_t count) const = 0;
	virtual bool GetInt16MonoAudio(int16_t *buf, int64_t start, int64_t count) const { return false; }
	virtual bool GetFloatAudioChannel(float *buf, int channel, int64_t start, int64_t count) const { return false; }
	virtual void HintFloatAudio(int64_t start, int64_t count) const { }
};

std::unique_ptr<AudioDisplaySource> CreateAudioDisplaySource(agi::AudioProvider *provider);
std::unique_ptr<AudioDisplaySource> CreateInt16MonoAudioDisplaySource(agi::AudioProvider *provider);
std::unique_ptr<AudioDisplaySource> CreateSingleChannelAudioDisplaySource(AudioDisplaySource *source, int channel);
