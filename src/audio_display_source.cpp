#include "audio_display_source.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
class AudioProviderDisplaySource final : public AudioDisplaySource {
	agi::AudioProvider *provider;
	mutable std::vector<char> raw_buffer;
	mutable std::vector<int16_t> s16_buffer;
	mutable std::vector<int32_t> s32_buffer;
	mutable std::vector<double> f64_buffer;

	static float DecodeUInt8(uint8_t sample) {
		return static_cast<float>(static_cast<int>(sample) - 128) / 128.0f;
	}

	static float DecodeInt16(const char *ptr) {
		int16_t sample;
		std::memcpy(&sample, ptr, sizeof(sample));
		return static_cast<float>(sample) / 32768.0f;
	}

	static float DecodeInt24(const char *ptr) {
		int32_t sample = (static_cast<unsigned char>(ptr[0])) |
			(static_cast<unsigned char>(ptr[1]) << 8) |
			(static_cast<unsigned char>(ptr[2]) << 16);
		if (sample & 0x00800000)
			sample |= ~0x00FFFFFF;
		return static_cast<float>(sample) / 8388608.0f;
	}

	static float DecodeInt32(const char *ptr) {
		int32_t sample;
		std::memcpy(&sample, ptr, sizeof(sample));
		return static_cast<float>(sample / 2147483648.0);
	}

	static float DecodeFloat32(const char *ptr) {
		float sample;
		std::memcpy(&sample, ptr, sizeof(sample));
		return sample;
	}

	static float DecodeFloat64(const char *ptr) {
		double sample;
		std::memcpy(&sample, ptr, sizeof(sample));
		return static_cast<float>(sample);
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

	void GetFloatAudio(float *buf, int64_t start, int64_t count) const override {
		if (!provider || !buf || count <= 0)
			return;

		int channels = std::max(1, provider->GetChannels());
		int bytes_per_sample = std::max(1, provider->GetBytesPerSample());
		size_t sample_count = static_cast<size_t>(count) * channels;
		if (provider->AreSamplesFloat()) {
			if (bytes_per_sample == 4) {
				provider->GetAudio(buf, start, count);
			}
			else if (bytes_per_sample == 8) {
				f64_buffer.resize(sample_count);
				provider->GetAudio(f64_buffer.data(), start, count);
				for (size_t i = 0; i < sample_count; ++i)
					buf[i] = static_cast<float>(f64_buffer[i]);
			}
			else {
				std::fill(buf, buf + sample_count, 0.f);
			}
		}
		else {
			switch (bytes_per_sample) {
				case 1:
					raw_buffer.resize(sample_count);
					provider->GetAudio(raw_buffer.data(), start, count);
					for (size_t i = 0; i < sample_count; ++i)
						buf[i] = DecodeUInt8(static_cast<uint8_t>(raw_buffer[i]));
					break;
				case 2:
					s16_buffer.resize(sample_count);
					provider->GetAudio(s16_buffer.data(), start, count);
					for (size_t i = 0; i < sample_count; ++i)
						buf[i] = static_cast<float>(s16_buffer[i]) / 32768.0f;
					break;
				case 3:
					raw_buffer.resize(sample_count * bytes_per_sample);
					provider->GetAudio(raw_buffer.data(), start, count);
					{
						const char *src = raw_buffer.data();
					for (size_t i = 0; i < sample_count; ++i)
						buf[i] = DecodeInt24(src + i * bytes_per_sample);
					}
					break;
				case 4:
					s32_buffer.resize(sample_count);
					provider->GetAudio(s32_buffer.data(), start, count);
					for (size_t i = 0; i < sample_count; ++i)
						buf[i] = static_cast<float>(s32_buffer[i] / 2147483648.0);
					break;
				default:
					std::fill(buf, buf + sample_count, 0.f);
					break;
			}
		}
	}
};
}

std::unique_ptr<AudioDisplaySource> CreateAudioDisplaySource(agi::AudioProvider *provider) {
	if (!provider)
		return nullptr;
	return agi::make_unique<AudioProviderDisplaySource>(provider);
}
