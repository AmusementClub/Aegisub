#pragma once

#include <wx/event.h>
#include <wx/spinctrl.h>

#include <cstdint>
#include <functional>
#include <utility>

namespace aegisub::motion_track {

enum class DialogOptionControl : std::uint8_t {
	Choice,
	Spin,
	Check
};

/// Text edits arrive before a spin control commits its numeric value. Both
/// events invalidate a preview immediately, without trying to read a partial
/// value or rebuild a plan while the user is still typing.
inline void BindDialogOptionChanges(wxEvtHandler& control, DialogOptionControl kind,
									std::function<void()> on_change) {
	auto changed = [on_change = std::move(on_change)](wxCommandEvent&) { on_change(); };
	switch (kind) {
		case DialogOptionControl::Choice:
			control.Bind(wxEVT_COMBOBOX, changed);
			break;
		case DialogOptionControl::Spin:
			control.Bind(wxEVT_SPINCTRL, changed);
			control.Bind(wxEVT_TEXT, changed);
			break;
		case DialogOptionControl::Check:
			control.Bind(wxEVT_CHECKBOX, changed);
			break;
	}
}

} // namespace aegisub::motion_track
