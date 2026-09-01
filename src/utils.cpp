// Copyright (c) 2005-2006, Rodrigo Braz Monteiro
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

#include "utils.h"

#include "compat.h"
#include "format.h"
#include "options.h"
#include "retina_helper.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#ifdef __UNIX__
#include <unistd.h>
#endif
#include <filesystem>
#include <map>
#include <unicode/locid.h>
#include <unicode/unistr.h>
#include <wx/clipbrd.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/window.h>

#ifdef __APPLE__
#include <libaegisub/util_osx.h>
#include <CoreText/CTFont.h>
#endif

wxCursor GetEyedropperCursor() {
#ifdef __WXMSW__
	// Named cursor resource from res.rc; only the MSW port loads cursors by
	// resource name.
	wxCursor eyedropper(wxS("eyedropper_cursor"));
	if (eyedropper.IsOk())
		return eyedropper;
#endif
	return *wxCROSS_CURSOR;
}

/// @brief There shall be no kiB, MiB stuff here Pretty reading of size
wxString PrettySize(int bytes) {
	const wxString suffix[] = {
		wxEmptyString, wxS("kB"), wxS("MB"), wxS("GB"), wxS("TB"),
		wxS("PB"), wxS("EB"), wxS("ZB"), wxS("YB")
	};

	// Set size
	size_t i = 0;
	double size = bytes;
	while (size > 1024 && i + 1 < sizeof(suffix) / sizeof(suffix[0])) {
		size /= 1024.0;
		i++;
	}

	// Set number of decimal places
	wxString fmt = wxS("%.0f");
	if (size < 10)
		fmt = wxS("%.2f");
	else if (size < 100)
		fmt = wxS("%1.f");
	return agi::wxformat(fmt, size) + wxS(" ") + suffix[i];
}

int SmallestPowerOf2(int x) {
	x--;
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	x++;
	return x;
}

#ifndef __WXMAC__
void RestartAegisub() {
	config::opt->Flush();

#if defined(__WXMSW__)
	wxExecute(wxS("\"") + wxStandardPaths::Get().GetExecutablePath() + wxS("\""));
#else
	wxExecute(wxStandardPaths::Get().GetExecutablePath());
#endif
}
#endif

bool ForwardMouseWheelEvent(wxWindow *source, wxMouseEvent &evt) {
	wxWindow *target = wxFindWindowAtPoint(wxGetMousePosition());
	if (!target || target == source) return true;

	// If the mouse is over a parent of the source window just pretend it's
	// over the source window, so that the mouse wheel works on borders and such
	wxWindow *parent = source->GetParent();
	while (parent && parent != target) parent = parent->GetParent();
	if (parent == target) return true;

	// Otherwise send it to the new target
	target->GetEventHandler()->ProcessEvent(evt);
	evt.Skip(false);
	return false;
}

bool IsVideoDpiScaled() {
	return OPT_GET("Video/Scale with DPI")->GetBool();
}

int ScaleVideoUi(wxWindow *window, int value) {
	return IsVideoDpiScaled() ? window->FromDIP(value) : value;
}

wxSize ScaleVideoUi(wxWindow *window, wxSize const& value) {
	return IsVideoDpiScaled() ? window->FromDIP(value) : value;
}

int GetVideoUiIconSize(wxWindow *window, int logical_size) {
	return window->FromDIP(logical_size);
}

double GetWindowScaleFactor(wxWindow *window) {
#ifdef __WXMSW__
	return window->GetDPIScaleFactor();
#else
	return window->GetContentScaleFactor();
#endif
}

std::string GetClipboard() {
	wxString data;
	wxClipboard *cb = wxClipboard::Get();
	wxLogNull disable_logging;
	for (int i = 0; i < 5; ++i) {
		if (cb->Open()) {
			if (cb->IsSupported(wxDF_TEXT) || cb->IsSupported(wxDF_UNICODETEXT)) {
				wxTextDataObject raw_data;
				cb->GetData(raw_data);
				data = raw_data.GetText();
			}
			cb->Close();
			break;
		}
		wxMilliSleep(20);
	}
	return from_wx(data);
}

void SetClipboard(std::string const& new_data) {
	wxClipboard *cb = wxClipboard::Get();
	wxLogNull disable_logging;
	for (int i = 0; i < 5; ++i) {
		if (cb->Open()) {
			cb->SetData(new wxTextDataObject(to_wx(new_data)));
			cb->Flush();
			cb->Close();
			break;
		}
		wxMilliSleep(20);
	}
}

void SetClipboard(wxBitmap const& new_data) {
	wxClipboard *cb = wxClipboard::Get();
	wxLogNull disable_logging;
	for (int i = 0; i < 5; ++i) {
		if (cb->Open()) {
			cb->SetData(new wxBitmapDataObject(new_data));
			cb->Flush();
			cb->Close();
			break;
		}
		wxMilliSleep(20);
	}
}

void TextControlClipboardCharHook(wxKeyEvent &event, bool editable) {
	// Block parent CHAR_HOOK hotkeys (edit/line/copy etc.) without suppressing
	// native KEY_DOWN/CHAR — see wxEvent::DoAllowNextEvent.
	if (!event.CmdDown() || event.AltDown()) {
		event.Skip();
		return;
	}

	int key = event.GetUnicodeKey();
	if (key >= 'a' && key <= 'z')
		key -= 'a' - 'A';
	if (key != 'C' && key != 'X' && key != 'V') {
		event.Skip();
		return;
	}

	// Not Skip(): stop FrameMain / DialogDetachedVideo hooks.
	// DoAllowNextEvent(): still generate normal key events for the control.
	if (editable || key == 'C')
		event.DoAllowNextEvent();
}

#ifndef __WXOSX_COCOA__
// OS X implementation in osx_utils.mm
void AddFullScreenButton(wxWindow *) { }
void SetFloatOnParent(wxWindow *) { }

// OS X implementation in retina_helper.mm
RetinaHelper::RetinaHelper(wxWindow* w) { window = w; }
RetinaHelper::~RetinaHelper() { }
int RetinaHelper::GetScaleFactor() const {
#ifdef __WXGTK__
	return int(window->GetContentScaleFactor());
#else
	return 1;
#endif
}
#endif

wxString FontFace(std::string opt_prefix) {
	opt_prefix += "/Font Face";
	auto value = OPT_GET(opt_prefix)->GetString();
#ifdef __WXOSX_COCOA__
	if (value.empty()) {
		auto default_font = CTFontCreateUIFontForLanguage(kCTFontUserFontType, 0, nullptr);
		auto default_font_name = CTFontCopyPostScriptName(default_font);
		CFRelease(default_font);

		auto utf8_str = CFStringGetCStringPtr(default_font_name, kCFStringEncodingUTF8);
		if (utf8_str)
			value = utf8_str;
		else {
			char buffer[1024];
			CFStringGetCString(default_font_name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
			buffer[1023] = '\0';
			value = buffer;
		}

		CFRelease(default_font_name);
	}
#endif
	return to_wx(value);
}

static wxString ResolveFileDialogPath(std::string const& option_name, std::string const& default_path) {
	std::string path;
	if (!default_path.empty())
		path = default_path;
	else if (!option_name.empty())
		path = OPT_GET(option_name)->GetString();

	if (path.empty())
		return wxString();

	if (config::path)
		path = agi::fs::PathToString(config::path->Decode(path));
	return to_wx(path);
}

static void UpdateFileDialogOption(std::string const& option_name, agi::fs::path const& filename) {
	if (!filename.empty() && !option_name.empty())
		OPT_SET(option_name)->SetString(config::path && option_name.rfind("Path/Last/", 0) != 0
			? config::path->Encode(filename.parent_path())
			: agi::fs::PathToString(filename.parent_path()));
}

static agi::fs::path FileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, int flags, wxWindow *parent) {
	auto path = ResolveFileDialogPath(option_name, default_path);
	agi::fs::path filename = wxFileSelector(message, path, to_wx(default_filename), to_wx(default_extension), to_wx(wildcard), flags, parent).wx_str();
	UpdateFileDialogOption(option_name, filename);
	return filename;
}

static std::vector<agi::fs::path> FilesSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, int flags, wxWindow *parent) {
	wxFileDialog dialog(parent, message, ResolveFileDialogPath(option_name, default_path), to_wx(default_filename), to_wx(wildcard), flags);
	if (dialog.ShowModal() == wxID_CANCEL)
		return {};

	wxArrayString selections;
	dialog.GetPaths(selections);

	std::vector<agi::fs::path> paths;
	paths.reserve(selections.size());
	for (auto const& selection : selections)
		paths.emplace_back(selection.wx_str());

	if (!paths.empty())
		UpdateFileDialogOption(option_name, paths.front());
	return paths;
}

agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return OpenFileSelector(message, option_name, "", default_filename, default_extension, wildcard, parent, true);
}

std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return OpenFilesSelector(message, option_name, "", default_filename, default_extension, wildcard, parent, true);
}

agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return SaveFileSelector(message, option_name, "", default_filename, default_extension, wildcard, parent, true);
}

agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist) {
	return OpenFileSelector(message, option_name, "", default_filename, default_extension, wildcard, parent, must_exist);
}

std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist) {
	return OpenFilesSelector(message, option_name, "", default_filename, default_extension, wildcard, parent, must_exist);
}

agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool prompt_overwrite) {
	return SaveFileSelector(message, option_name, "", default_filename, default_extension, wildcard, parent, prompt_overwrite);
}

agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return OpenFileSelector(message, option_name, default_path, default_filename, default_extension, wildcard, parent, true);
}

std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return OpenFilesSelector(message, option_name, default_path, default_filename, default_extension, wildcard, parent, true);
}

agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return SaveFileSelector(message, option_name, default_path, default_filename, default_extension, wildcard, parent, true);
}

agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist) {
	int flags = wxFD_OPEN;
	if (must_exist)
		flags |= wxFD_FILE_MUST_EXIST;
	return FileSelector(message, option_name, default_path, default_filename, default_extension, wildcard, flags, parent);
}

std::vector<agi::fs::path> OpenFilesSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool must_exist) {
	int flags = wxFD_OPEN | wxFD_MULTIPLE;
	if (must_exist)
		flags |= wxFD_FILE_MUST_EXIST;
	return FilesSelector(message, option_name, default_path, default_filename, default_extension, wildcard, flags, parent);
}

agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_path, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent, bool prompt_overwrite) {
	int flags = wxFD_SAVE;
	if (prompt_overwrite)
		flags |= wxFD_OVERWRITE_PROMPT;
	return FileSelector(message, option_name, default_path, default_filename, default_extension, wildcard, flags, parent);
}

agi::fs::path SelectDirectorySelector(wxString const& message, std::string const& default_path, wxWindow *parent) {
	return from_wx(wxDirSelector(message, ResolveFileDialogPath("", default_path), 0, wxDefaultPosition, parent));
}

wxString LocalizedLanguageName(wxString const& lang) {
	icu::Locale iculoc(lang.utf8_string().c_str());
	if (!iculoc.isBogus()) {
		icu::UnicodeString ustr;
		iculoc.getDisplayName(iculoc, ustr);
#ifdef _MSC_VER
		return wxString((const wchar_t*)ustr.getBuffer());
#else
		std::string utf8;
		ustr.toUTF8String(utf8);
		return to_wx(utf8);
#endif
	}

	if (auto info = wxLocale::FindLanguageInfo(lang))
		return info->Description;
	return lang;
}
