#include "gui_wx_runtime_host.h"
#include "gui_wx_subtitle_format_host.h"
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
		},
		[] {
			RegisterGuiWxSubtitleFormats();
		},
	};
}
