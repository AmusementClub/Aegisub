#include <main.h>

#include "../../src/utils.h"

#include <wx/event.h>

namespace {

// wxKeyEvent has no setters for the key code or Unicode character, but the
// corresponding data members are public for compatibility.
wxKeyEvent MakeHookEvent(int key, bool control_down, bool alt_down) {
	wxKeyEvent event(wxEVT_CHAR_HOOK);
	event.m_keyCode = key;
	event.m_uniChar = static_cast<wxChar>(key);
	event.SetControlDown(control_down);
	event.SetAltDown(alt_down);
	return event;
}

} // namespace

TEST(TextControlClipboardCharHook, editable_clipboard_keys_stay_in_the_control) {
	for (int key : {'C', 'X', 'V'}) {
		auto event = MakeHookEvent(key, true, false);
		TextControlClipboardCharHook(event, true);
		EXPECT_FALSE(event.GetSkipped())
			<< "Ctrl+" << static_cast<char>(key) << " must not reach parent CHAR_HOOKs";
		EXPECT_TRUE(event.IsNextEventAllowed())
			<< "Ctrl+" << static_cast<char>(key) << " must still reach the native control";
	}
}

TEST(TextControlClipboardCharHook, lowercase_keys_are_treated_as_clipboard_keys) {
	auto event = MakeHookEvent('x', true, false);
	TextControlClipboardCharHook(event, true);
	EXPECT_FALSE(event.GetSkipped());
	EXPECT_TRUE(event.IsNextEventAllowed());
}

TEST(TextControlClipboardCharHook, readonly_allows_copy_but_swallows_cut_and_paste) {
	auto copy = MakeHookEvent('C', true, false);
	TextControlClipboardCharHook(copy, false);
	EXPECT_FALSE(copy.GetSkipped());
	EXPECT_TRUE(copy.IsNextEventAllowed());

	for (int key : {'X', 'V'}) {
		auto event = MakeHookEvent(key, true, false);
		TextControlClipboardCharHook(event, false);
		EXPECT_FALSE(event.GetSkipped())
			<< "Ctrl+" << static_cast<char>(key) << " must not reach parent CHAR_HOOKs";
		EXPECT_FALSE(event.IsNextEventAllowed())
			<< "Ctrl+" << static_cast<char>(key) << " must not touch a read-only control";
	}
}

TEST(TextControlClipboardCharHook, non_clipboard_keys_propagate_to_parent_hotkeys) {
	// initializer_list elements are const, which would make the hook mutate a
	// compiler-generated copy instead of the event, so use a mutable array.
	wxKeyEvent events[] = {
		MakeHookEvent('A', true, false),        // Ctrl+A is not a clipboard key
		MakeHookEvent('X', false, false),       // no modifier
		MakeHookEvent('X', true, true),         // Ctrl+Alt+X is not plain Cmd
		MakeHookEvent(WXK_RETURN, true, false), // non-printable key
	};
	for (auto& event : events) {
		TextControlClipboardCharHook(event, true);
		EXPECT_TRUE(event.GetSkipped());
	}
}
