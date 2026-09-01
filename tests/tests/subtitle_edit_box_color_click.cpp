#include <main.h>

#include "../../src/subtitle_edit_box_color_click.h"

namespace {

using aegisub::subtitle_edit_box_color_click::Action;
using aegisub::subtitle_edit_box_color_click::ColorClickSequencer;

// Button identity is the pick command the button would reroute to.
const void *const primary = "edit/color/primary/pick/video";
const void *const outline = "edit/color/outline/pick/video";

TEST(SubsEditBoxColorClick, single_click_opens_dialog_after_window_closes) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	EXPECT_TRUE(seq.pending());
	EXPECT_EQ(Action::OpenDialog, seq.OnWindowExpired());
	EXPECT_FALSE(seq.pending());
}

// wxMSW delivers one physical double-click press to both bindings:
// wxAnyButton::MSWWindowProc forwards WM_LBUTTONDBLCLK to the base once as a
// synthesized WM_LBUTTONDOWN and then processes the real double click. The
// synthesized down must not disturb the pending cycle, and the native click
// the button reports for that release must be swallowed instead of re-arming
// the dialog after the quick pick already ran.
TEST(SubsEditBoxColorClick, msw_double_press_reroutes_once_and_swallows_extra_click) {
	ColorClickSequencer seq;

	seq.OnPlainPress();                                       // first press
	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary)); // first release arms
	seq.OnPlainPress();                                       // synthesized down
	EXPECT_EQ(Action::RunPick, seq.OnDoublePress(primary));   // real double click
	EXPECT_FALSE(seq.pending());
	EXPECT_EQ(Action::SwallowEvent, seq.OnButtonClick(primary)); // release's extra click
	EXPECT_EQ(Action::None, seq.OnWindowExpired());              // no dialog afterwards
}

// GTK (2BUTTON_PRESS) and macOS (clickCount >= 2) deliver the second press
// as only the double click, with no preceding plain down of their own.
TEST(SubsEditBoxColorClick, paired_double_press_without_synthesized_down_reroutes) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	EXPECT_EQ(Action::RunPick, seq.OnDoublePress(primary));
	EXPECT_FALSE(seq.pending());
}

// Ports report the second press as a plain down once the mouse moved past
// the system double-click rectangle; two ordinary clicks must keep ordinary
// click behaviour instead of rerouting.
TEST(SubsEditBoxColorClick, plain_second_press_keeps_ordinary_click_behaviour) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	seq.OnPlainPress();                                       // unpaired second press
	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary)); // its release re-arms
	EXPECT_EQ(Action::OpenDialog, seq.OnWindowExpired());     // dialog opens once
}

// A rerouted press released outside its button never produces the click that
// would consume the swallow debt; the next physical press retires it so the
// following ordinary click is not eaten.
TEST(SubsEditBoxColorClick, swallow_debt_from_outside_release_retires_on_next_press) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	EXPECT_EQ(Action::RunPick, seq.OnDoublePress(primary));
	seq.OnPlainPress();                                       // next gesture's press
	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary)); // not eaten by the debt
}

// A double click with nothing deferred (e.g. the third press of a triple
// click) is not the sequencer's gesture.
TEST(SubsEditBoxColorClick, double_press_with_nothing_pending_passes_through) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::PassEvent, seq.OnDoublePress(primary));
	EXPECT_FALSE(seq.pending());
}

TEST(SubsEditBoxColorClick, double_press_for_other_button_passes_through) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	EXPECT_EQ(Action::PassEvent, seq.OnDoublePress(outline));
	EXPECT_TRUE(seq.pending());
	EXPECT_EQ(Action::OpenDialog, seq.OnWindowExpired());
}

TEST(SubsEditBoxColorClick, repeated_plain_clicks_defer_each_newest_release) {
	ColorClickSequencer seq;

	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	seq.OnPlainPress();
	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(primary));
	seq.OnPlainPress();
	EXPECT_EQ(Action::DeferOpen, seq.OnButtonClick(outline));
	EXPECT_EQ(Action::OpenDialog, seq.OnWindowExpired());
}

}
