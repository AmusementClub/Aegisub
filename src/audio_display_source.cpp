#include "audio_display_source.h"
#include "simd/audio_sample_convert.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct AudioDecodeScratch {
	std::vector<char> raw_buffer;
	std::vector<int16_t> s16_buffer;
	std::vector<int32_t> s32_buffer;
	std::vector<double> f64_buffer;
	std::vector<float> f32_buffer;
};

AudioDecodeScratch& GetAudioDecodeScratch() {
	thread_local AudioDecodeScratch scratch;
	return scratch;
}

void ExtractInterleavedChannel(const float *src, int channels, int channel, int64_t count, float *dst) {
	const float *cur = src + channel;
	for (int64_t i = 0; i < count; ++i, cur += channels)
		dst[i] = *cur;
}

class AudioProviderDisplaySource final : public AudioDisplaySource {
	agi::AudioProvider *provider;

	static float DecodeUInt8(uint8_t sample) {
		return static_cast<float>(static_cast<int>(sample) - 128) / 128.0f;
	}

	static float DecodeInt24(const char *ptr) {
		int32_t sample = (static_cast<unsigned char>(ptr[0])) |
			(static_cast<unsigned char>(ptr[1]) << 8) |
			(static_cast<unsigned char>(ptr[2]) << 16);
		if (sample & 0x00800000)
			sample |= ~0x00FFFFFF;
		return static_cast<float>(sample) / 8388608.0f;
	}

public:
	AudioProviderDisplaySource(agi::AudioProvider *provider)
	: provider(provider) {
	}

	int64_t GetNumSamples() const override {
		return provider ? provider->GetNumSamples() : 0;
	}

	int GetChannels() const override {
		return provider ? provider->GetChannels() : 0;
	}

	int GetSampleRate() const override {
		return provider ? provider->GetSampleRate() : 0;
	}

	void HintFloatAudio(int64_t start, int64_t count) const override {
		if (provider && count > 0)
			provider->HintVisibleRange(start, count);
	}

	void GetFloatAudio(float *buf, int64_t start, int64_t count) const override {
		if (!provider || !buf || count <= 0)
			return;

		auto &scratch = GetAudioDecodeScratch();
		int channels = std::max(1, provider->GetChannels());
		int bytes_per_sample = std::max(1, provider->GetBytesPerSample());
		size_t sample_count = static_cast<size_t>(count) * channels;
		if (provider->AreSamplesFloat()) {
			if (bytes_per_sample == 4) {
				provider->GetAudio(buf, start, count);
			}
			else if (bytes_per_sample == 8) {
				scratch.f64_buffer.resize(sample_count);
				provider->GetAudio(scratch.f64_buffer.data(), start, count);
				aegisub::simd::DecodeFloat64ToFloat(scratch.f64_buffer.data(), sample_count, buf);
			}
			else {
				std::fill(buf, buf + sample_count, 0.f);
			}
		}
		else {
			switch (bytes_per_sample) {
				case 1:
					scratch.raw_buffer.resize(sample_count);
					provider->GetAudio(scratch.raw_buffer.data(), start, count);
					aegisub::simd::DecodeUInt8ToFloat(reinterpret_cast<uint8_t const*>(scratch.raw_buffer.data()), sample_count, buf);
					break;
				case 2:
					scratch.s16_buffer.resize(sample_count);
					provider->GetAudio(scratch.s16_buffer.data(), start, count);
					aegisub::simd::DecodeInt16ToFloat(scratch.s16_buffer.data(), sample_count, buf);
					break;
				case 3:
					scratch.raw_buffer.resize(sample_count * bytes_per_sample);
					provider->GetAudio(scratch.raw_buffer.data(), start, count);
					{
						const char *src = scratch.raw_buffer.data();
						for (size_t i = 0; i < sample_count; ++i)
							buf[i] = DecodeInt24(src + i * bytes_per_sample);
					}
					break;
				case 4:
					scratch.s32_buffer.resize(sample_count);
					provider->GetAudio(scratch.s32_buffer.data(), start, count);
					aegisub::simd::DecodeInt32ToFloat(scratch.s32_buffer.data(), sample_count, buf);
					break;
				default:
					std::fill(buf, buf + sample_count, 0.f);
					break;
			}
		}
	}

	bool GetFloatAudioChannel(float *buf, int channel, int64_t start, int64_t count) const override {
		if (!provider || !buf || count <= 0)
			return false;

		const int channels = std::max(1, provider->GetChannels());
		if (channel < 0 || channel >= channels) {
			std::fill(buf, buf + count, 0.f);
			return true;
		}
		if (channels == 1) {
			GetFloatAudio(buf, start, count);
			return true;
		}

		auto &scratch = GetAudioDecodeScratch();
		const int bytes_per_sample = std::max(1, provider->GetBytesPerSample());
		const size_t sample_count = static_cast<size_t>(count) * channels;
		if (provider->AreSamplesFloat()) {
			if (bytes_per_sample == 4) {
				scratch.f32_buffer.resize(sample_count);
				provider->GetAudio(scratch.f32_buffer.data(), start, count);
				ExtractInterleavedChannel(scratch.f32_buffer.data(), channels, channel, count, buf);
				return true;
			}
			if (bytes_per_sample == 8) {
				scratch.f64_buffer.resize(sample_count);
				provider->GetAudio(scratch.f64_buffer.data(), start, count);
				const double *src = scratch.f64_buffer.data() + channel;
				for (int64_t i = 0; i < count; ++i, src += channels)
					buf[i] = static_cast<float>(*src);
				return true;
			}
			std::fill(buf, buf + count, 0.f);
			return true;
		}

		switch (bytes_per_sample) {
			case 1: {
				scratch.raw_buffer.resize(sample_count);
				provider->GetAudio(scratch.raw_buffer.data(), start, count);
				const uint8_t *src = reinterpret_cast<uint8_t const*>(scratch.raw_buffer.data()) + channel;
				for (int64_t i = 0; i < count; ++i, src += channels)
					buf[i] = DecodeUInt8(*src);
				return true;
			}
			case 2: {
				scratch.s16_buffer.resize(sample_count);
				provider->GetAudio(scratch.s16_buffer.data(), start, count);
				const int16_t *src = scratch.s16_buffer.data() + channel;
				for (int64_t i = 0; i < count; ++i, src += channels)
					buf[i] = static_cast<float>(*src) / 32768.0f;
				return true;
			}
			case 3: {
				scratch.raw_buffer.resize(sample_count * bytes_per_sample);
				provider->GetAudio(scratch.raw_buffer.data(), start, count);
				const char *src = scratch.raw_buffer.data() + static_cast<ptrdiff_t>(channel) * bytes_per_sample;
				for (int64_t i = 0; i < count; ++i, src += static_cast<ptrdiff_t>(channels) * bytes_per_sample)
					buf[i] = DecodeInt24(src);
				return true;
			}
			case 4: {
				scratch.s32_buffer.resize(sample_count);
				provider->GetAudio(scratch.s32_buffer.data(), start, count);
				const int32_t *src = scratch.s32_buffer.data() + channel;
				for (int64_t i = 0; i < count; ++i, src += channels)
					buf[i] = static_cast<float>(*src / 2147483648.0);
				return true;
			}
			default:
				std::fill(buf, buf + count, 0.f);
				return true;
		}
	}
};
}

std::unique_ptr<AudioDisplaySource> CreateAudioDisplaySource(agi::AudioProvider *provider) {
	if (!provider)
		return nullptr;
	return agi::make_unique<AudioProviderDisplaySource>(provider);
}

namespace {
class SingleChannelAudioDisplaySource final : public AudioDisplaySource {
	AudioDisplaySource *core;
	int channel;
	int total_channels;
public:
	SingleChannelAudioDisplaySource(AudioDisplaySource *source, int ch)
		: core(source), channel(ch), total_channels(std::max(1, source->GetChannels())) {}
	int64_t GetNumSamples() const override { return core->GetNumSamples(); }
	int GetChannels() const override { return 1; }
	int GetSampleRate() const override { return core->GetSampleRate(); }
	void HintFloatAudio(int64_t start, int64_t count) const override {
		core->HintFloatAudio(start, count);
	}
	void GetFloatAudio(float *buf, int64_t start, int64_t count) const override {
		if (!buf || count <= 0) return;
		if (total_channels == 1) { core->GetFloatAudio(buf, start, count); return; }
		if (core->GetFloatAudioChannel(buf, channel, start, count))
			return;
		auto &scratch = GetAudioDecodeScratch();
		scratch.f32_buffer.resize(static_cast<size_t>(count) * total_channels);
		core->GetFloatAudio(scratch.f32_buffer.data(), start, count);
		ExtractInterleavedChannel(scratch.f32_buffer.data(), total_channels, channel, count, buf);
	}
};
}

std::unique_ptr<AudioDisplaySource> CreateSingleChannelAudioDisplaySource(AudioDisplaySource *source, int channel) {
	if (!source) return nullptr;
	return std::make_unique<SingleChannelAudioDisplaySource>(source, channel);
}
