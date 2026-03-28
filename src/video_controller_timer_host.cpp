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

#include "video_controller_timer_host.h"

#include "main_thread_timer.h"

#include <utility>

namespace {

class DispatchVideoControllerTimerHost final : public VideoControllerTimerHost {
	MainThreadTimer playback_timer;

public:
	explicit DispatchVideoControllerTimerHost(std::function<void()> on_play_timer)
	: playback_timer(std::move(on_play_timer)) {
	}

	void Start(int interval_ms) override {
		playback_timer.StartRepeating(interval_ms);
	}

	void Stop() override {
		playback_timer.Stop();
	}

	bool IsRunning() const override {
		return playback_timer.IsRunning();
	}
};

}

std::unique_ptr<VideoControllerTimerHost> CreateVideoControllerTimerHost(
	std::function<void()> on_play_timer) {
	return std::make_unique<DispatchVideoControllerTimerHost>(std::move(on_play_timer));
}
