// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "audio_controller_timer.h"

#include "threaded_ui_timer.h"
#include "ui_timer.h"

#include <utility>

namespace {

std::shared_ptr<UiTimerHost> ResolveUiTimerHost() {
	if (auto host = GetUiTimerHost())
		return host;
	return CreateThreadedUiTimerHost();
}

class DispatchAudioControllerTimer final : public AudioControllerTimer {
	std::unique_ptr<UiTimer> playback_timer;

public:
	explicit DispatchAudioControllerTimer(std::function<void()> on_playback_timer)
	: playback_timer(ResolveUiTimerHost()->CreateTimer(std::move(on_playback_timer))) {
	}

	void Start(int interval_ms) override {
		playback_timer->StartRepeating(interval_ms);
	}

	void Stop() override {
		playback_timer->Stop();
	}
};

}

std::unique_ptr<AudioControllerTimer> CreateAudioControllerTimer(
	std::function<void()> on_playback_timer) {
	return std::make_unique<DispatchAudioControllerTimer>(std::move(on_playback_timer));
}
