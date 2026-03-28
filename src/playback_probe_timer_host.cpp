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

#include "playback_probe_timer_host.h"

#include <wx/timer.h>

#include <utility>

namespace aegisub::playback_probe_service {
namespace {

class WxPlaybackProbeTimerHost final : public PlaybackProbeTimerHost, public wxEvtHandler {
	std::function<void()> on_timeout;
	std::function<void()> on_completion_poll;
	std::function<void()> on_restart_timer;
	std::function<void()> on_seek_timer;
	wxTimer timeout_timer{this};
	wxTimer completion_timer{this};
	wxTimer restart_timer{this};
	wxTimer seek_timer{this};

	void HandleTimeout(wxTimerEvent&) {
		if (on_timeout)
			on_timeout();
	}

	void HandleCompletionPoll(wxTimerEvent&) {
		if (on_completion_poll)
			on_completion_poll();
	}

	void HandleRestartTimer(wxTimerEvent&) {
		if (on_restart_timer)
			on_restart_timer();
	}

	void HandleSeekTimer(wxTimerEvent&) {
		if (on_seek_timer)
			on_seek_timer();
	}

public:
	WxPlaybackProbeTimerHost(
		std::function<void()> on_timeout,
		std::function<void()> on_completion_poll,
		std::function<void()> on_restart_timer,
		std::function<void()> on_seek_timer)
	: on_timeout(std::move(on_timeout))
	, on_completion_poll(std::move(on_completion_poll))
	, on_restart_timer(std::move(on_restart_timer))
	, on_seek_timer(std::move(on_seek_timer)) {
		Bind(wxEVT_TIMER, &WxPlaybackProbeTimerHost::HandleTimeout, this, timeout_timer.GetId());
		Bind(wxEVT_TIMER, &WxPlaybackProbeTimerHost::HandleCompletionPoll, this, completion_timer.GetId());
		Bind(wxEVT_TIMER, &WxPlaybackProbeTimerHost::HandleRestartTimer, this, restart_timer.GetId());
		Bind(wxEVT_TIMER, &WxPlaybackProbeTimerHost::HandleSeekTimer, this, seek_timer.GetId());
	}

	void StopAll() override {
		timeout_timer.Stop();
		completion_timer.Stop();
		restart_timer.Stop();
		seek_timer.Stop();
	}

	void StartTimeoutOnce(int timeout_ms) override {
		timeout_timer.Start(timeout_ms, true);
	}

	void StartCompletionPolling(int interval_ms) override {
		completion_timer.Start(interval_ms);
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

std::unique_ptr<PlaybackProbeTimerHost> CreatePlaybackProbeTimerHost(
	std::function<void()> on_timeout,
	std::function<void()> on_completion_poll,
	std::function<void()> on_restart_timer,
	std::function<void()> on_seek_timer) {
	return std::make_unique<WxPlaybackProbeTimerHost>(
		std::move(on_timeout),
		std::move(on_completion_poll),
		std::move(on_restart_timer),
		std::move(on_seek_timer));
}

}
