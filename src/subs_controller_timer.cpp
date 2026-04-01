#include "subs_controller_timer.h"

#include <wx/timer.h>
#include <wx/event.h>

namespace {

class WxSubsControllerTimer final : public SubsControllerTimer {
	wxTimer timer;
public:
	WxSubsControllerTimer(std::function<void()> on_timer) {
		timer.Bind(wxEVT_TIMER, [on_timer = std::move(on_timer)](wxTimerEvent&) {
			on_timer();
		});
	}

	void Start(int interval_ms) override { timer.Start(interval_ms); }
	void Stop() override { timer.Stop(); }
	bool IsRunning() const override { return timer.IsRunning(); }
};

class NullSubsControllerTimer final : public SubsControllerTimer {
public:
	void Start(int) override { }
	void Stop() override { }
	bool IsRunning() const override { return false; }
};

} // namespace

std::unique_ptr<SubsControllerTimer> CreateSubsControllerTimer(
	std::function<void()> on_timer) {
	return std::make_unique<WxSubsControllerTimer>(std::move(on_timer));
}

std::unique_ptr<SubsControllerTimer> CreateNullSubsControllerTimer() {
	return std::make_unique<NullSubsControllerTimer>();
}
