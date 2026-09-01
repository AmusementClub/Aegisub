#pragma once

#include <cstdint>

namespace aegisub::subtitle_edit_box_color_click {

/// What the edit box should do with the event it fed to the sequencer.
enum class Action : std::uint8_t {
	/// The gesture does not concern the sequencer.
	None,
	/// Let the event continue into default handling (wxMouseEvent::Skip).
	PassEvent,
	/// Consume the event so the native control never sees it.
	SwallowEvent,
	/// A lone click: keep the dialog command deferred and open the
	/// double-click window.
	DeferOpen,
	/// The window closed without a second press: open the dialog.
	OpenDialog,
	/// The port-paired second press: run the pick-from-video command; the
	/// press itself stays swallowed.
	RunPick,
};

/// Resolves the single-vs-double click gesture of the colour buttons.
///
/// The button's native control reports every completed press/release as a
/// click, so the first click's dialog command stays deferred until the
/// double-click window closes, and the second press of a double click
/// reroutes to the pick-from-video command instead. Only the port-paired
/// double click (wxEVT_LEFT_DCLICK) reroutes: every major port pairs the
/// second press into one (the Windows BUTTON class is registered with
/// CS_DBLCLKS, GTK reports 2BUTTON_PRESS, macOS reports clickCount >= 2),
/// while a plain wxEVT_LEFT_DOWN means the port did not pair the presses —
/// which is exactly what the second ordinary click looks like once the mouse
/// moved past the system double-click rectangle, and it must keep ordinary
/// click behaviour.
///
/// On wxMSW one physical double-click press reaches both handlers:
/// wxAnyButton::MSWWindowProc feeds WM_LBUTTONDBLCLK through the base once
/// as a synthesized WM_LBUTTONDOWN and then again as the real double click.
/// The plain-press entry therefore only retires stale swallow debt; it never
/// touches the pending cycle the double-press entry is about to finish, and
/// the extra click the native button reports for that release is consumed
/// by the debt instead of re-arming the dialog.
class ColorClickSequencer {
	public:
	/// A wxEVT_LEFT_DOWN: a press the port did not pair into a double click.
	/// Always passes through; the caller just skips the event afterwards.
	/// Its only job is to retire swallow debt left by a rerouted press that
	/// was released outside its button, so the debt cannot eat the next
	/// ordinary click.
	void OnPlainPress() noexcept {
		swallow_debt_ = 0;
	}

	/// A wxEVT_LEFT_DCLICK: the port paired this press with the previous
	/// click. `button` identifies the pick command of the pressed button;
	/// only a press on the button whose click is still deferred reroutes.
	Action OnDoublePress(const void *button) noexcept {
		if (!pending_ || button != pending_button_) {
			return Action::PassEvent;
		}
		pending_ = false;
		pending_button_ = nullptr;
		++swallow_debt_;
		return Action::RunPick;
	}

	/// A wxEVT_BUTTON: the button reports a completed press/release cycle.
	/// `button` identifies the pick command of the reporting button and
	/// becomes the pairing target while the click stays deferred.
	Action OnButtonClick(const void *button) noexcept {
		if (swallow_debt_ > 0) {
			--swallow_debt_;
			return Action::SwallowEvent;
		}
		pending_ = true;
		pending_button_ = button;
		return Action::DeferOpen;
	}

	/// The deferred-click window closed without a second press.
	Action OnWindowExpired() noexcept {
		if (!pending_) {
			return Action::None;
		}
		pending_ = false;
		pending_button_ = nullptr;
		return Action::OpenDialog;
	}

	/// Is a click currently deferred, waiting for the window to close?
	[[nodiscard]] bool pending() const noexcept {
		return pending_;
	}

	private:
	bool pending_ = false;
	const void *pending_button_ = nullptr;
	int swallow_debt_ = 0;
};

}
