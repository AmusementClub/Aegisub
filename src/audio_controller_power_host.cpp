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
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "audio_controller_power_host.h"

#include "app_runtime.h"

#include <wx/app.h>
#include <wx/power.h>

#include <utility>

namespace {

class NoopAudioControllerPowerHost final : public AudioControllerPowerHost {
};

#ifdef wxHAS_POWER_EVENTS
class WxAudioControllerPowerHost final : public AudioControllerPowerHost, public wxEvtHandler {
	wxEvtHandler *event_source = nullptr;
	std::function<void()> on_suspend;
	std::function<void()> on_resume;

	void HandleSuspend(wxPowerEvent&) {
		if (on_suspend)
			on_suspend();
	}

	void HandleResume(wxPowerEvent&) {
		if (on_resume)
			on_resume();
	}

public:
	WxAudioControllerPowerHost(
		wxEvtHandler *event_source,
		std::function<void()> on_suspend,
		std::function<void()> on_resume)
	: event_source(event_source)
	, on_suspend(std::move(on_suspend))
	, on_resume(std::move(on_resume)) {
		if (!this->event_source)
			return;
		this->event_source->Bind(wxEVT_POWER_SUSPENDED, &WxAudioControllerPowerHost::HandleSuspend, this);
		this->event_source->Bind(wxEVT_POWER_RESUME, &WxAudioControllerPowerHost::HandleResume, this);
	}

	~WxAudioControllerPowerHost() override {
		if (!event_source)
			return;
		event_source->Unbind(wxEVT_POWER_SUSPENDED, &WxAudioControllerPowerHost::HandleSuspend, this);
		event_source->Unbind(wxEVT_POWER_RESUME, &WxAudioControllerPowerHost::HandleResume, this);
	}
};
#endif

}

std::unique_ptr<AudioControllerPowerHost> CreateAudioControllerPowerHost(
	std::function<void()> on_suspend,
	std::function<void()> on_resume) {
#ifdef wxHAS_POWER_EVENTS
	if (IsGuiRuntimeShell() && wxTheApp) {
		return std::make_unique<WxAudioControllerPowerHost>(
			wxTheApp,
			std::move(on_suspend),
			std::move(on_resume));
	}
#else
	(void)on_suspend;
	(void)on_resume;
#endif
	return std::make_unique<NoopAudioControllerPowerHost>();
}
