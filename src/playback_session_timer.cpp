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

#include "playback_session_timer.h"

#include "main_thread_timer.h"

#include <utility>

namespace aegisub::playback_session_service {
namespace {

class DispatchPlaybackSessionTimer final : public PlaybackSessionTimer {
	MainThreadTimer delay_timer;
	MainThreadTimer wait_timer;

public:
	DispatchPlaybackSessionTimer(
		std::function<void()> on_delay_timer,
		std::function<void()> on_wait_timer)
	: delay_timer(std::move(on_delay_timer))
	, wait_timer(std::move(on_wait_timer)) {
	}

	void StopAll() override {
		delay_timer.Stop();
		wait_timer.Stop();
	}

	void StartDelayOnce(int delay_ms) override {
		delay_timer.StartOnce(delay_ms);
	}

	void StartWaitPolling(int interval_ms) override {
		wait_timer.StartRepeating(interval_ms);
	}

	void StopWaitPolling() override {
		wait_timer.Stop();
	}
};

}

std::unique_ptr<PlaybackSessionTimer> CreatePlaybackSessionTimer(
	std::function<void()> on_delay_timer,
	std::function<void()> on_wait_timer) {
	return std::make_unique<DispatchPlaybackSessionTimer>(
		std::move(on_delay_timer),
		std::move(on_wait_timer));
}

}
