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

#include "playback_probe_timer.h"

#include "main_thread_timer.h"

#include <utility>

namespace aegisub::playback_probe_service {
namespace {

class DispatchPlaybackProbeTimer final : public PlaybackProbeTimer {
	MainThreadTimer timeout_timer;
	MainThreadTimer completion_timer;
	MainThreadTimer restart_timer;
	MainThreadTimer seek_timer;

public:
	DispatchPlaybackProbeTimer(
		std::function<void()> on_timeout,
		std::function<void()> on_completion_poll,
		std::function<void()> on_restart_timer,
		std::function<void()> on_seek_timer)
	: timeout_timer(std::move(on_timeout))
	, completion_timer(std::move(on_completion_poll))
	, restart_timer(std::move(on_restart_timer))
	, seek_timer(std::move(on_seek_timer)) {
	}

	void StopAll() override {
		timeout_timer.Stop();
		completion_timer.Stop();
		restart_timer.Stop();
		seek_timer.Stop();
	}

	void StartTimeoutOnce(int timeout_ms) override {
		timeout_timer.StartOnce(timeout_ms);
	}

	void StartCompletionPolling(int interval_ms) override {
		completion_timer.StartRepeating(interval_ms);
	}

	void StartRestartOnce(int delay_ms) override {
		restart_timer.StartOnce(delay_ms);
	}

	void ArmSeek(std::optional<int> delay_ms) override {
		seek_timer.Stop();
		if (delay_ms)
			seek_timer.StartOnce(*delay_ms);
	}
};

}

std::unique_ptr<PlaybackProbeTimer> CreatePlaybackProbeTimer(
	std::function<void()> on_timeout,
	std::function<void()> on_completion_poll,
	std::function<void()> on_restart_timer,
	std::function<void()> on_seek_timer) {
	return std::make_unique<DispatchPlaybackProbeTimer>(
		std::move(on_timeout),
		std::move(on_completion_poll),
		std::move(on_restart_timer),
		std::move(on_seek_timer));
}

}
