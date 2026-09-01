// Copyright (c) 2005-2009, Rodrigo Braz Monteiro, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <libaegisub/fs_fwd.h>

#include "cache_cleanup.h"
#include "numeric_utils.h"

#include <cstdint>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/cursor.h>
#include <wx/string.h>

class wxKeyEvent;
class wxMouseEvent;
class wxWindow;

wxString PrettySize(int bytes);

/// @brief Get the smallest power of two that is greater or equal to x
///
/// Algorithm from http://bob.allegronetwork.com/prog/tricks.html
int SmallestPowerOf2(int x);

/// @brief Launch a new copy of Aegisub.
///
/// Contrary to what the name suggests, this does not close the currently
/// running process.
void RestartAegisub();

/// Add the OS X 10.7+ full-screen button to a window
void AddFullScreenButton(wxWindow *window);

void SetFloatOnParent(wxWindow *window);

/// Forward a mouse wheel event to the window under the mouse if needed
/// @param source The initial target of the wheel event
/// @param evt The event
/// @return Should the calling code process the event?
bool ForwardMouseWheelEvent(wxWindow *source, wxMouseEvent &evt);

/// The eyedropper cursor used by every colour-sampling mode, so the video
/// quick pick and the colour picker's screen dropper look the same. Falls back
/// to a crosshair on ports without the bundled cursor resource.
wxCursor GetEyedropperCursor();

bool IsVideoDpiScaled();
int ScaleVideoUi(wxWindow *window, int value);
wxSize ScaleVideoUi(wxWindow *window, wxSize const& value);
int GetVideoUiIconSize(wxWindow *window, int logical_size = 16);
double GetWindowScaleFactor(wxWindow *window);

/// Get the text contents of the clipboard, or empty string on failure
std::string GetClipboard();
/// Try to set the clipboard to the given string
void SetClipboard(std::string const& new_value);
void SetClipboard(wxBitmap const& new_value);

/// Handle Ctrl+C/X/V on a control CHAR_HOOK so frame-level hotkeys cannot steal them.
///
/// Does not Skip clipboard keys (blocks parent CHAR_HOOK / edit/line/*), but
/// calls DoAllowNextEvent() so the native control still gets KEY_DOWN/CHAR and
/// performs WM_COPY/CUT/PASTE with correct selection semantics.
///
/// @param editable If false, Ctrl+C is allowed through natively but Ctrl+X/V
///                 are swallowed so they cannot fall through to edit/line/*.
void TextControlClipboardCharHook(wxKeyEvent &event, bool editable);

#define countof(array) (sizeof(array) / sizeof(array[0]))

wxString FontFace(std::string opt_prefix);

agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent);
std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent);
agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent);
agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist);
std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist);
agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool prompt_overwrite);
agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent);
std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent);
agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent);
agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist);
std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist);
agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool prompt_overwrite);
agi::fs::path SelectDirectorySelector(wxString const& message, std::string const& default_path, wxWindow *parent);

wxString LocalizedLanguageName(wxString const& lang);

#ifdef __WXOSX__
namespace osx {
	/// Make the given menu the OS X Window menu
	void make_windows_menu(wxMenu *wxmenu);
	/// Activate a top-level document window other than the given one
	bool activate_top_window_other_than(wxFrame *wx);
	// Bring all windows to the front, maintaining relative z-order
	void bring_to_front();
}
#endif
