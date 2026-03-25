// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace agi {
struct AudioProviderMemoryStats {
	std::string provider_name;
	std::string storage_kind;
	size_t storage_bytes = 0;
	size_t logical_bytes = 0;
	size_t decoded_bytes = 0;
	size_t page_size_bytes = 0;
	size_t loading_bytes = 0;
	size_t pinned_bytes = 0;
	size_t free_bytes = 0;
	int64_t num_samples = 0;
	int64_t decoded_samples = 0;
	int64_t resident_pages = 0;
	int64_t loading_pages = 0;
	int64_t pinned_pages = 0;
	int64_t free_pages = 0;
	int sample_rate = 0;
	int bytes_per_sample = 0;
	int channels = 0;
	bool float_samples = false;
};

class AudioProvider {
protected:
	int channels = 0;
	/// Total number of samples per channel
	int64_t num_samples = 0;
	/// Samples per channel which have been decoded and can be fetched with FillBuffer
	/// Only applicable for the cache providers
	std::atomic<int64_t> decoded_samples{0};
	int sample_rate = 0;
	int bytes_per_sample = 0;
	bool float_samples = false;

	virtual void FillBuffer(void *buf, int64_t start, int64_t count) const = 0;
	virtual void FillBufferInt16Mono(int16_t* buf, int64_t start, int64_t count) const;

	void ZeroFill(void *buf, int64_t count) const;
	AudioProviderMemoryStats BuildMemoryStats(std::string provider_name = {}, std::string storage_kind = {}, size_t storage_bytes = 0) const;

public:
	virtual ~AudioProvider() = default;

	void GetAudio(void *buf, int64_t start, int64_t count) const;
	void GetInt16MonoAudio(int16_t* buf, int64_t start, int64_t count) const;
	void GetInt16MonoAudioWithVolume(int16_t *buf, int64_t start, int64_t count, double volume) const;

	int64_t GetNumSamples()     const { return num_samples; }
	int64_t GetDecodedSamples() const { return decoded_samples; }
	int     GetSampleRate()     const { return sample_rate; }
	int     GetBytesPerSample() const { return bytes_per_sample; }
	int     GetChannels()       const { return channels; }
	bool    AreSamplesFloat()   const { return float_samples; }
	virtual AudioProviderMemoryStats GetMemoryStats() const;
	virtual void SetPlaybackWindow(int64_t current_frame, int64_t ahead_frames, int64_t behind_frames) { }
	virtual void ClearPlaybackWindow() { }
	virtual void HintVisibleRange(int64_t start_frame, int64_t frame_count) { }

	/// Does this provider benefit from external caching?
	virtual bool NeedsCache() const { return false; }
};

/// Helper base class for an audio provider which wraps another provider
class AudioProviderWrapper : public AudioProvider {
protected:
	std::unique_ptr<AudioProvider> source;
public:
	AudioProviderWrapper(std::unique_ptr<AudioProvider> src)
	: source(std::move(src))
	{
		channels = source->GetChannels();
		num_samples = source->GetNumSamples();
		decoded_samples = source->GetDecodedSamples();
		sample_rate = source->GetSampleRate();
		bytes_per_sample = source->GetBytesPerSample();
		float_samples = source->AreSamplesFloat();
	}

	AudioProviderMemoryStats GetMemoryStats() const override;
	void SetPlaybackWindow(int64_t current_frame, int64_t ahead_frames, int64_t behind_frames) override {
		if (source)
			source->SetPlaybackWindow(current_frame, ahead_frames, behind_frames);
	}
	void ClearPlaybackWindow() override {
		if (source)
			source->ClearPlaybackWindow();
	}
	void HintVisibleRange(int64_t start_frame, int64_t frame_count) override {
		if (source)
			source->HintVisibleRange(start_frame, frame_count);
	}
};

DEFINE_EXCEPTION(AudioProviderError, Exception);

/// Error of some sort occurred while decoding a frame
DEFINE_EXCEPTION(AudioDecodeError, AudioProviderError);

/// This provider could not find any audio data in the file
DEFINE_EXCEPTION(AudioDataNotFound, AudioProviderError);

class BackgroundRunner;

std::unique_ptr<AudioProvider> CreateDummyAudioProvider(fs::path const& filename, BackgroundRunner *);
std::unique_ptr<AudioProvider> CreatePCMAudioProvider(fs::path const& filename, BackgroundRunner *);

std::unique_ptr<AudioProvider> CreateConvertAudioProvider(std::unique_ptr<AudioProvider> source_provider);
std::unique_ptr<AudioProvider> CreateLockAudioProvider(std::unique_ptr<AudioProvider> source_provider);
std::unique_ptr<AudioProvider> CreateHDAudioProvider(std::unique_ptr<AudioProvider> source_provider, fs::path const& dir);
std::unique_ptr<AudioProvider> CreateRAMAudioProvider(std::unique_ptr<AudioProvider> source_provider);

void SaveAudioClip(AudioProvider const& provider, fs::path const& path, int start_time, int end_time);
}
