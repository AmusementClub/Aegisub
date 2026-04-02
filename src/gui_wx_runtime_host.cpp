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

#include "gui_wx_runtime_host.h"
#include "ui_timer.h"

// GUI runtime bring-up is isolated here so AppRuntime host hooks are provided
// by an explicit wx shell rather than being inlined into main.cpp.
#include <algorithm>
#include <utility>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/timer.h>

namespace {

class GuiWxTimerImpl final : public wxTimer {
	std::function<void()> callback;

public:
	explicit GuiWxTimerImpl(std::function<void()> callback)
	: callback(std::move(callback)) {
	}

	void Notify() override {
		if (callback)
			callback();
	}
};

class GuiWxUiTimer final : public UiTimer {
	GuiWxTimerImpl timer;

public:
	explicit GuiWxUiTimer(std::function<void()> callback)
	: timer(std::move(callback)) {
	}

	~GuiWxUiTimer() override {
		timer.Stop();
	}

	void StartOnce(int delay_ms) override {
		timer.Start(std::max(0, delay_ms), true);
	}

	void StartRepeating(int interval_ms) override {
		timer.Start(std::max(0, interval_ms), false);
	}

	void Stop() override {
		timer.Stop();
	}

	bool IsRunning() const override {
		return timer.IsRunning();
	}
};

class GuiWxUiTimerHost final : public UiTimerHost {
public:
	std::unique_ptr<UiTimer> CreateTimer(std::function<void()> callback) override {
		return std::make_unique<GuiWxUiTimer>(std::move(callback));
	}
};

}

std::shared_ptr<UiTimerHost> CreateGuiWxUiTimerHost() {
	return std::make_shared<GuiWxUiTimerHost>();
}

RuntimeProcessHost BuildGuiWxRuntimeProcessHost() {
	return {
		[] {
			(void)wxLog::GetActiveTarget();
		},
	};
}

RuntimeOptionalFacilityHost BuildGuiWxRuntimeOptionalFacilityHost() {
	return {
		[] {
			wxImage::AddHandler(new wxPNGHandler);
		}
	};
}
