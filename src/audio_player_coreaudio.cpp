// Copyright (c) 2022, Qirui Wang
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#ifdef WITH_COREAUDIO
#include "include/aegisub/audio_player.h"

#include <mutex>
#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioQueue.h>

#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

namespace {
class CoreAudioPlayer final : public AudioPlayer {
    static const int num_buffers = 8;
    AudioQueueRef aq = nullptr;
    AudioQueueBufferRef buffer[num_buffers];
    AudioStreamBasicDescription format;

    int64_t samples_per_buffer;
    int64_t next_input_frame = 0, start_frame = 0, end_frame = 0;
    Float64 start_sample, sample_rate = 0;
    bool original = true;
    bool playing = false;
    std::mutex mutex;
    static void Callback(void *self, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);
    void FillBuffer(AudioQueueRef inAQ, AudioQueueBufferRef buffer);
    Float64 GetSampleTime();
public:
    CoreAudioPlayer(agi::AudioProvider *provider);
    ~CoreAudioPlayer();

    void Play(int64_t start, int64_t count) override;
    void Stop() override;
    bool IsPlaying() override { return playing; }

    void SetVolume(double vol) override { AudioQueueSetParameter(aq, kAudioQueueParam_Volume, vol); }

    int64_t GetEndPosition() override { return end_frame; }
    int64_t GetCurrentPosition() override;
    void SetEndPosition(int64_t pos) override;
};

CoreAudioPlayer::CoreAudioPlayer(agi::AudioProvider *provider)
: AudioPlayer(provider)
{
    format.mSampleRate = provider->GetSampleRate();
    format.mFormatID = kAudioFormatLinearPCM;
    if (provider->AreSamplesFloat()) {
        format.mFormatFlags = kAudioFormatFlagIsFloat;
    } else if (provider->GetBytesPerSample() == sizeof(uint8_t)) {
        format.mFormatFlags = kAudioFormatFlagsAreAllClear;
    } else {
        format.mFormatFlags = kAudioFormatFlagIsSignedInteger;
    }
    format.mBytesPerPacket = provider->GetChannels() * provider->GetBytesPerSample();
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBytesPerPacket;
    format.mChannelsPerFrame = provider->GetChannels();
    format.mBitsPerChannel = provider->GetBytesPerSample() * 8;
    format.mReserved = 0;
    OSStatus ret = AudioQueueNewOutput(&format, Callback, this, NULL, NULL, 0, &aq);
    if (ret) {
        if (ret == kAudioFormatUnsupportedDataFormatError) {
            original = false;
            format.mFormatFlags = kAudioFormatFlagIsSignedInteger;
            format.mBytesPerPacket = sizeof(int16_t);
            format.mFramesPerPacket = 1;
            format.mBytesPerFrame = sizeof(int16_t);
            format.mChannelsPerFrame = 1;
            format.mBitsPerChannel = sizeof(int16_t) * 8;
            ret = AudioQueueNewOutput(&format, Callback, this, NULL, NULL, 0, &aq);
            if (ret) {
                throw AudioPlayerOpenError("AudioQueueNewOutput failed");
            }
        } else {
            throw AudioPlayerOpenError("AudioQueueNewOutput failed");
        }
    }
    samples_per_buffer = provider->GetSampleRate() / num_buffers / 2;
    for (int i = 0; i < num_buffers; ++i) {
        AudioQueueAllocateBuffer(aq, samples_per_buffer * format.mBytesPerFrame, buffer + i);
    }
}

CoreAudioPlayer::~CoreAudioPlayer()
{
    std::lock_guard<std::mutex> lock(mutex);
    Stop();
    AudioQueueDispose(aq, false);
    aq = nullptr;
}

void CoreAudioPlayer::Callback(void *self, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
    CoreAudioPlayer *player = static_cast<CoreAudioPlayer*>(self);
    std::lock_guard<std::mutex> lock(player->mutex);
    player->FillBuffer(inAQ, inBuffer);
}

void CoreAudioPlayer::FillBuffer(AudioQueueRef inAQ, AudioQueueBufferRef buffer)
{
    if (!playing) {
        LOG_D("audio/player/coreaudio") << "FillBuffer quit: not playing";
        return;
    }
    int fill_len = std::min<int>(end_frame - next_input_frame, samples_per_buffer);
    if (fill_len <= 0) {
        LOG_D("audio/player/coreaudio") << "FillBuffer quit: zero fill_len";
        return;
    }
    if (original) {
        provider->GetAudio(buffer->mAudioData, next_input_frame, fill_len);
    } else {
        provider->GetInt16MonoAudio(reinterpret_cast<int16_t*>(buffer->mAudioData), next_input_frame, fill_len);
    }
    next_input_frame += fill_len;
    buffer->mAudioDataByteSize = fill_len * format.mBytesPerFrame;
    buffer->mPacketDescriptionCount = 0;
    buffer->mUserData = NULL;
    AudioQueueEnqueueBuffer(inAQ, buffer, 0, NULL);
}

Float64 CoreAudioPlayer::GetSampleTime()
{
    AudioTimeStamp ts;
    AudioQueueDeviceGetCurrentTime(aq, &ts);
    if (ts.mFlags & kAudioTimeStampSampleTimeValid)
        return ts.mSampleTime;
    return 0;
}

void CoreAudioPlayer::Play(int64_t start, int64_t count)
{
    playing = false;
    AudioQueueReset(aq);

    start_frame = start;
    end_frame = start + count;
    next_input_frame = start;
    playing = true;
    OSStatus ret = AudioQueueStart(aq, NULL);
    if (ret) {
        throw AudioPlayerOpenError("AudioQueueStart failed");
    }
    for (int i = 0; i < num_buffers; ++i) {
        FillBuffer(aq, buffer[i]);
    }
    start_sample = GetSampleTime();
    LOG_D("audio/player/coreaudio") << "Playback begun";
}

void CoreAudioPlayer::Stop()
{
    LOG_D("audio/player/coreaudio") << "Stop";
    playing = false;
    AudioQueueStop(aq, true);
}

int64_t CoreAudioPlayer::GetCurrentPosition()
{
    Float64 samples = GetSampleTime() - start_sample;
    if (sample_rate == 0) {
        UInt32 size = sizeof(sample_rate);
        AudioQueueGetProperty(aq, kAudioQueueDeviceProperty_SampleRate, &sample_rate, &size);
    }
    if (sample_rate == 0) {
        return start_frame;
    }
    return samples / sample_rate * provider->GetSampleRate() + start_frame;
}

void CoreAudioPlayer::SetEndPosition(int64_t pos)
{
    end_frame = pos;
    if (end_frame <= GetCurrentPosition())
        Stop();
}

}

std::unique_ptr<AudioPlayer> CreateCoreAudioPlayer(agi::AudioProvider *provider, wxWindow *)
{
    return agi::make_unique<CoreAudioPlayer>(provider);
}

#endif // WITH_COREAUDIO
