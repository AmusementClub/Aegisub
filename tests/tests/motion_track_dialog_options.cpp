#include <main.h>

#include "../../src/motion_track/dialog_option_events.h"

#include <memory>

namespace {
using namespace aegisub::motion_track;

class motion_track_dialog_options : public testing::Test {
	protected:
	wxEvtHandler control;
	std::shared_ptr<const int> preview = std::make_shared<const int>(42);
	bool checked = true;
	int paints = 0;

	void Bind(DialogOptionControl kind) {
		BindDialogOptionChanges(control, kind, [this] {
			preview.reset();
			checked = false;
			++paints;
		});
	}

	void ExpectInvalidated() const {
		EXPECT_EQ(nullptr, preview);
		EXPECT_FALSE(checked);
		EXPECT_EQ(1, paints);
	}
};
} // namespace

TEST_F(motion_track_dialog_options, changing_choice_invalidates_preview_before_apply) {
	Bind(DialogOptionControl::Choice);
	wxCommandEvent event(wxEVT_COMBOBOX);
	control.ProcessEvent(event);
	ExpectInvalidated();
}

TEST_F(motion_track_dialog_options, changing_checkbox_invalidates_preview_before_apply) {
	Bind(DialogOptionControl::Check);
	wxCommandEvent event(wxEVT_CHECKBOX);
	control.ProcessEvent(event);
	ExpectInvalidated();
}

TEST_F(motion_track_dialog_options, typing_partial_spin_value_invalidates_preview_immediately) {
	Bind(DialogOptionControl::Spin);
	wxCommandEvent event(wxEVT_TEXT);
	event.SetString(wxString::FromUTF8("-"));
	control.ProcessEvent(event);
	ExpectInvalidated();
}

TEST_F(motion_track_dialog_options, clicking_spin_arrow_invalidates_preview_immediately) {
	Bind(DialogOptionControl::Spin);
	wxSpinEvent event(wxEVT_SPINCTRL);
	event.SetPosition(3);
	control.ProcessEvent(event);
	ExpectInvalidated();
}

TEST_F(motion_track_dialog_options, unrelated_events_keep_preview_visible) {
	Bind(DialogOptionControl::Spin);
	wxCommandEvent event(wxEVT_BUTTON);
	control.ProcessEvent(event);
	ASSERT_NE(nullptr, preview);
	EXPECT_EQ(42, *preview);
	EXPECT_TRUE(checked);
	EXPECT_EQ(0, paints);
}
