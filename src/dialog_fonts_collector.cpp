// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "font_collector_core.h"

#include "compat.h"
#include "dialog_manager.h"
#include "format.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "stc_compat.h"
#include "ui_dispatch.h"
#include "ui_services.h"
#include "utils.h"
#include "value_event.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/make_unique.h>

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

namespace {
class DialogFontsCollector final : public wxDialog {
	agi::Context *context;
	AssFile *subs;
	agi::Path &path;
	FontCollectionMode mode = FontCollectionMode::CheckFontsOnly;
	FontCollectorMatcher matcher = FontCollectorMatcher::Platform;
	agi::ui::UiActivationScope ui_activation;

	wxStyledTextCtrl *collection_log;
	wxButton *close_btn;
	wxButton *dest_browse_button;
	wxButton *start_btn;
	wxRadioBox *collection_mode;
	wxRadioBox *font_matcher;
	wxStaticText *dest_label;
	wxTextCtrl *dest_ctrl;

	bool goto_end_on_idle = false;

	void OnStart(wxCommandEvent &);
	void OnBrowse(wxCommandEvent &);
	void OnRadio(wxCommandEvent &e);

	/// Append text to log message from worker thread
	void OnAddText(ValueEvent<std::pair<int, wxString>>& event);
	/// Collection complete notification from the worker thread to reenable buttons
	void OnCollectionComplete(wxThreadEvent &);
	void OnIdle(wxIdleEvent&);

	void UpdateControls();

public:
	DialogFontsCollector(agi::Context *c);
	~DialogFontsCollector() override { ui_activation.Deactivate(); }
	agi::ui::WeakLifetime GetAsyncUiLifetime() const { return ui_activation.GetLifetime(); }
};

using color_str_pair = std::pair<int, wxString>;
wxDEFINE_EVENT(EVT_ADD_TEXT, ValueEvent<color_str_pair>);
wxDEFINE_EVENT(EVT_COLLECTION_DONE, wxThreadEvent);

std::string FormatMissingGlyphs(std::string const& str) {
	std::string printable;
	std::string unprintable;
	size_t i = 0;
	while (i < str.size()) {
		UChar32 c;
		U8_NEXT((const uint8_t*)str.data(), i, (int32_t)str.size(), c);
		if (!u_isUWhiteSpace(c)) {
			printable += str.substr(i - U8_LENGTH(c), U8_LENGTH(c));
		}
		else {
			char buf[64];
			snprintf(buf, sizeof buf, "\n - U+%04X ", c);
			unprintable += buf;
			UErrorCode ec = U_ZERO_ERROR;
			char name[1024];
			auto len = u_charName(c, U_EXTENDED_CHAR_NAME, name, sizeof name, &ec);
			if (len != 0 && U_SUCCESS(ec))
				unprintable += name;
			if (c == 0xA0)
				unprintable += " (\\h)";
		}
	}

	return printable + unprintable;
}

wxString FormatLineList(std::vector<int> const& lines) {
	wxString text;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i)
			text += wxS(", ");
		text += fmt_wx("%d", lines[i]);
	}
	return text;
}

color_str_pair FormatFontCollectorEvent(FontCollectorEvent const& event) {
	switch (event.type) {
		case FontCollectorEventType::FontBackendInfo:
			return {0, fmt_wx("Font provider: %s\n", event.message)};
		case FontCollectorEventType::UpdatingFontCache:
			return {0, _("Updating font cache\n")};
		case FontCollectorEventType::FontCacheError:
			return {3, to_wx(event.message + "\n")};
		case FontCollectorEventType::ParsingFile:
			return {0, _("Parsing file\n")};
		case FontCollectorEventType::StyleMissing:
			if (event.lines.size() == 1)
				return {2, fmt_tl("Style '%s' does not exist on line %d\n", event.style, event.lines.front())};
			if (event.lines.size() > 1)
				return {2, fmt_tl("Style '%s' does not exist on lines: %s\n", event.style, FormatLineList(event.lines))};
			return {2, fmt_tl("Style '%s' does not exist\n", event.style)};
		case FontCollectorEventType::SearchingForFontFiles:
			return {0, _("Searching for font files\n")};
		case FontCollectorEventType::FontMissing:
			return {2, fmt_tl("Could not find font '%s'\n", event.face)};
		case FontCollectorEventType::FontFound: {
			if (event.message == "memory")
				return {0, fmt_tl("Found '%s' in memory; it will be dumped when collecting.\n", event.face)};
			auto src = event.message.empty() ? wxString{} : fmt_wx(" [%s]", event.message);
			return {0, fmt_tl("Found '%s' at '%s'%s\n", event.face, event.path, src)};
		}
		case FontCollectorEventType::FakeBold: {
			wxString weight_hint = event.requested_weight
			    ? fmt_wx(" (requested weight %d)", event.requested_weight)
			    : wxString{};
			return {3, fmt_tl("'%s' does not have a bold variant%s.\n", event.face, weight_hint)};
		}
		case FontCollectorEventType::FakeItalic:
			return {3, fmt_tl("'%s' does not have an italic variant.\n", event.face)};
		case FontCollectorEventType::MissingGlyphs:
			if (event.count > 50)
				return {2, fmt_tl("'%s' is missing %d glyphs used.\n", event.face, event.count)};
			return {2, fmt_tl("'%s' is missing the following glyphs used: %s\n", event.face, FormatMissingGlyphs(event.message))};
		case FontCollectorEventType::Usage: {
			wxString text;
			if (event.styles.size()) {
				text += _("Used in styles:\n");
				for (auto const& style : event.styles)
					text += fmt_wx("  - %s\n", style);
			}

			if (event.lines.size()) {
				text += _("Used on lines:");
				for (int line : event.lines)
					text += fmt_wx(" %d", line);
				text += wxS("\n");
			}
			text += wxS("\n");
			return {2, text};
		}
		case FontCollectorEventType::SearchComplete:
			return {0, _("Done\n\n")};
		case FontCollectorEventType::AllFontsFound:
			return {1, _("All fonts found.\n")};
		case FontCollectorEventType::FontsMissing:
			return {2, fmt_plural(event.count, "One font could not be found\n", "%d fonts could not be found.\n", event.count)};
		case FontCollectorEventType::FontsMissingGlyphs:
			return {2, fmt_plural(event.count,
				"One font was found, but was missing glyphs used in the script.\n",
				"%d fonts were found, but were missing glyphs used in the script.\n",
				event.count)};
		case FontCollectorEventType::CollectionCopyingFontsToFolder:
			return {0, _("Copying fonts to folder...\n")};
		case FontCollectorEventType::CollectionCopyingFontsToArchive:
			return {0, _("Copying fonts to archive...\n")};
		case FontCollectorEventType::CollectionFailedCreateDirectory:
			return {2, fmt_tl("* Failed to create directory '%s': %s.\n", event.path.wstring(), to_wx(event.message))};
		case FontCollectorEventType::CollectionFailedOpen:
			return {2, fmt_tl("* Failed to open %s.\n", event.path)};
		case FontCollectorEventType::CollectionCopied:
			return {1, fmt_tl("* Copied %s.\n", event.path)};
		case FontCollectorEventType::CollectionAlreadyExists:
			return {3, fmt_tl("* %s already exists on destination.\n", event.path.filename())};
		case FontCollectorEventType::CollectionFailedCopy:
			return {2, fmt_tl("* Failed to copy %s.\n", event.path)};
		case FontCollectorEventType::CollectionDoneAllCopied:
			return {1, _("Done. All fonts copied.")};
		case FontCollectorEventType::CollectionDoneSomeNotCopied:
			return {2, _("Done. Some fonts could not be copied.")};
		case FontCollectorEventType::CollectionNewline:
			return {0, wxS("\n")};
	}

	return {0, wxString{}};
}

class WxZipArchiveWriter final : public FontCollectionArchiveWriter {
	std::unique_ptr<wxFFileOutputStream> out;
	std::unique_ptr<wxZipOutputStream> zip;

public:
	WxZipArchiveWriter(agi::fs::path const& destination)
	: out(agi::make_unique<wxFFileOutputStream>(destination.wstring()))
	{
		if (out->IsOk())
			zip = agi::make_unique<wxZipOutputStream>(*out);
	}

	bool IsOk() const override {
		return out && out->IsOk() && zip && zip->IsOk();
	}

	bool AddFile(agi::fs::path const& source, agi::fs::path const& name) override {
		wxFFileInputStream in(source.wstring());
		if (!in.IsOk())
			return false;

		if (!zip->PutNextEntry(name.wstring()))
			return false;

		zip->Write(in);
		return zip->IsOk();
	}

	bool AddMemory(agi::fs::path const& name, std::span<char const> data) override {
		if (!zip->PutNextEntry(name.wstring()))
			return false;
		zip->Write(data.data(), data.size());
		return zip->IsOk();
	}
};

void FontsCollectorThread(AssFile *subs, agi::fs::path const& destination, FontCollectionMode oper,
                          FontCollectorMatcher matcher, wxEvtHandler *collector,
                          agi::ui::WeakLifetime lifetime) {
	agi::dispatch::BackgroundExecutor().Post([=]{
		auto AppendFontEvent = [&](FontCollectorEvent const& event) {
			agi::ui::MainAsyncIfAlive(lifetime, [collector, event] {
				collector->AddPendingEvent(ValueEvent<color_str_pair>(EVT_ADD_TEXT, -1, FormatFontCollectorEvent(event)));
			});
		};

		CollectFonts(subs, destination, oper, AppendFontEvent, nullptr, [](agi::fs::path const& archive) {
			return agi::make_unique<WxZipArchiveWriter>(archive);
		}, matcher);

		agi::ui::MainAsyncIfAlive(lifetime, [collector] {
			collector->AddPendingEvent(wxThreadEvent(EVT_COLLECTION_DONE));
		});
	});
}

DialogFontsCollector::DialogFontsCollector(agi::Context *c)
: wxDialog(c->GetUI().parent, -1, _("Fonts Collector"))
, context(c)
, subs(c->GetCore().ass.get())
, path(*c->GetCore().path)
{
	auto core = c->GetCore();
	SetIcon(GETICON(font_collector_button_16));

	wxString modes[] = {
		 _("Check fonts for availability")
		,_("Copy fonts to folder")
		,_("Copy fonts to subtitle file's folder")
		,_("Copy fonts to zipped archive")
	};

	mode = static_cast<FontCollectionMode>(mid<int>(0, OPT_GET("Tool/Fonts Collector/Action")->GetInt(), countof(modes) - 1));
	collection_mode = new wxRadioBox(this, -1, _("Action"), wxDefaultPosition, wxDefaultSize, countof(modes), modes, 1);
	collection_mode->SetSelection(static_cast<int>(mode));

	wxString matchers[] = {_("Platform"), wxS("libass")};
	matcher = static_cast<FontCollectorMatcher>(mid<int>(0, OPT_GET("Tool/Fonts Collector/Matcher")->GetInt(), countof(matchers) - 1));
	font_matcher = new wxRadioBox(this, -1, _("Font matcher"), wxDefaultPosition, wxDefaultSize, countof(matchers), matchers, 1);
	font_matcher->SetSelection(static_cast<int>(matcher));

	if (core.path->Decode("?script") == "?script")
		collection_mode->Enable(2, false);

	wxStaticBoxSizer *destination_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Destination"));

	dest_label = new wxStaticText(this, -1, wxS(" "));
	dest_ctrl = new wxTextCtrl(this, -1, to_wx(OPT_GET("Path/Fonts Collector Destination")->GetString()));
	dest_browse_button = new wxButton(this, -1, _("&Browse..."));

	wxSizer *dest_browse_sizer = new wxBoxSizer(wxHORIZONTAL);
	dest_browse_sizer->Add(dest_ctrl, wxSizerFlags(1).Border(wxRIGHT).Align(wxALIGN_CENTER_VERTICAL));
	dest_browse_sizer->Add(dest_browse_button, wxSizerFlags());

	destination_box->Add(dest_label, wxSizerFlags().Border(wxBOTTOM));
	destination_box->Add(dest_browse_sizer, wxSizerFlags().Expand());

	wxStaticBoxSizer *log_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Log"));
	collection_log = new wxStyledTextCtrl(this, -1, wxDefaultPosition, FromDIP(wxSize(600, 300)));
	aegisub::stc::ConfigureWindowsSelectionRendering(collection_log);
	collection_log->SetWrapMode(wxSTC_WRAP_WORD);
	collection_log->SetMarginWidth(1, 0);
	collection_log->SetReadOnly(true);
	collection_log->StyleSetForeground(1, wxColour(0, 200, 0));
	collection_log->StyleSetForeground(2, wxColour(200, 0, 0));
	collection_log->StyleSetForeground(3, wxColour(200, 100, 0));
	log_box->Add(collection_log, wxSizerFlags().Border());

	wxStdDialogButtonSizer *button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxHELP);
	start_btn = button_sizer->GetAffirmativeButton();
	close_btn = button_sizer->GetCancelButton();
	start_btn->SetLabel(_("&Start!"));
	start_btn->SetDefault();

	wxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(collection_mode, wxSizerFlags().Expand().Border());
	main_sizer->Add(font_matcher, wxSizerFlags().Expand().Border(wxALL & ~wxTOP));
	main_sizer->Add(destination_box, wxSizerFlags().Expand().Border(wxALL & ~wxTOP));
	main_sizer->Add(log_box, wxSizerFlags().Border(wxALL & ~wxTOP));
	main_sizer->Add(button_sizer, wxSizerFlags().Right().Border(wxALL & ~wxTOP));

	SetSizerAndFit(main_sizer);
	CenterOnParent();

	// Update the browse button and label
	UpdateControls();

	start_btn->Bind(wxEVT_BUTTON, &DialogFontsCollector::OnStart, this);
	dest_browse_button->Bind(wxEVT_BUTTON, &DialogFontsCollector::OnBrowse, this);
	collection_mode->Bind(wxEVT_RADIOBOX, &DialogFontsCollector::OnRadio, this);
	font_matcher->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent& event) {
		matcher = static_cast<FontCollectorMatcher>(event.GetInt());
		OPT_SET("Tool/Fonts Collector/Matcher")->SetInt(event.GetInt());
	});
	button_sizer->GetHelpButton()->Bind(wxEVT_BUTTON, std::bind(&HelpButton::OpenPage, "Fonts Collector"));
	Bind(EVT_ADD_TEXT, &DialogFontsCollector::OnAddText, this);
	Bind(EVT_COLLECTION_DONE, &DialogFontsCollector::OnCollectionComplete, this);
	Bind(wxEVT_IDLE, &DialogFontsCollector::OnIdle, this);
}

void DialogFontsCollector::OnStart(wxCommandEvent &) {
	collection_log->SetReadOnly(false);
	collection_log->ClearAll();
	collection_log->SetReadOnly(true);

	auto const destination_text = from_wx(dest_ctrl->GetValue());
	agi::fs::path dest;
	if (mode != FontCollectionMode::CheckFontsOnly) {
		dest = path.Decode(mode == FontCollectionMode::CopyToScriptFolder ? "?script/" : destination_text);

		auto destination_result = PrepareFontCollectionDestination(mode, dest);
		if (destination_result.invalid_destination)
			wxMessageBox(_("Invalid destination."), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);

		switch (destination_result.error) {
			case FontCollectionDestinationError::None:
				break;
			case FontCollectionDestinationError::CouldNotCreateDestinationFolder:
				wxMessageBox(_("Could not create destination folder."), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
				return;
			case FontCollectionDestinationError::InvalidArchivePath:
				wxMessageBox(_("Invalid path for .zip file."), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
				return;
		}
	}

	if (mode == FontCollectionMode::CopyToFolder || mode == FontCollectionMode::CopyToZip) {
		auto stored_destination = path.Encode(dest);
		if (!destination_text.empty() && destination_text[0] == '?')
			stored_destination = destination_text;
		OPT_SET("Path/Fonts Collector Destination")->SetString(stored_destination);
	}

	// Disable the UI while it runs as we don't support canceling
	EnableCloseButton(false);
	start_btn->Enable(false);
	dest_browse_button->Enable(false);
	dest_ctrl->Enable(false);
	close_btn->Enable(false);
	collection_mode->Enable(false);
	font_matcher->Enable(false);
	dest_label->Enable(false);

	FontsCollectorThread(subs, dest, mode, matcher, GetEventHandler(), GetAsyncUiLifetime());
}

void DialogFontsCollector::OnBrowse(wxCommandEvent &) {
	agi::fs::path dest;
	auto const current_dest = path.Decode(from_wx(dest_ctrl->GetValue()));
	if (mode == FontCollectionMode::CopyToZip) {
		auto current_path = wxFileName(current_dest.wstring());
		dest = context->RequestSaveFile({
			from_wx(_("Select archive file name")),
			"",
			from_wx(current_path.GetFullName()),
			".zip",
			"Zip Archives (*.zip)|*.zip",
			from_wx(current_path.GetPath())
		});
	}
	else
		dest = context->RequestSelectDirectory({
			from_wx(_("Select folder to save fonts on")),
			agi::fs::PathToString(current_dest)
		});

	if (!dest.empty())
		dest_ctrl->SetValue(to_wx(path.Encode(dest)));
}

void DialogFontsCollector::OnRadio(wxCommandEvent &evt) {
	OPT_SET("Tool/Fonts Collector/Action")->SetInt(evt.GetInt());
	mode = static_cast<FontCollectionMode>(evt.GetInt());
	UpdateControls();
}

void DialogFontsCollector::UpdateControls() {
	wxString dst = dest_ctrl->GetValue();

	if (mode == FontCollectionMode::CheckFontsOnly || mode == FontCollectionMode::CopyToScriptFolder) {
		dest_ctrl->Enable(false);
		dest_browse_button->Enable(false);
		dest_label->Enable(false);
		dest_label->SetLabel(_("N/A"));
	}
	else {
		dest_ctrl->Enable(true);
		dest_browse_button->Enable(true);
		dest_label->Enable(true);

		if (mode == FontCollectionMode::CopyToFolder) {
			dest_label->SetLabel(_("Choose the folder where the fonts will be collected to. It will be created if it doesn't exist."));

			// Remove filename from browse box
			if (dst.Right(4) == wxS(".zip"))
				dest_ctrl->SetValue(to_wx(agi::fs::PathToString(agi::fs::PathFromString(from_wx(dst)).parent_path())));
		}
		else {
			dest_label->SetLabel(_("Enter the name of the destination zip file to collect the fonts to. If a folder is entered, a default name will be used."));

			// Add filename to browse box
			if (!dst.EndsWith(wxS(".zip"))) {
				auto dest = agi::fs::PathFromString(from_wx(dst));
				dest /= "fonts.zip";
				dest_ctrl->SetValue(to_wx(agi::fs::PathToString(dest)));
			}
		}
	}

#ifdef __APPLE__
	// wxStaticText auto-wraps everywhere but OS X
	dest_label->Wrap(dest_label->GetParent()->GetSize().GetWidth() - 20);
	Layout();
#endif
}

void DialogFontsCollector::OnAddText(ValueEvent<color_str_pair> &event) {
	auto const& str = event.Get();
	collection_log->SetReadOnly(false);
	int pos = collection_log->GetLength();
	auto const& utf8 = str.second.utf8_str();
	collection_log->AppendTextRaw(utf8.data(), utf8.length());
	if (str.first) {
#if wxCHECK_VERSION (3, 1, 0)
		collection_log->StartStyling(pos);
#else
		collection_log->StartStyling(pos, 31);
#endif
		collection_log->SetStyling(utf8.length(), str.first);
	}
	collection_log->SetReadOnly(true);
	goto_end_on_idle = true;
}

void DialogFontsCollector::OnCollectionComplete(wxThreadEvent &) {
	EnableCloseButton(true);
	start_btn->Enable();
	close_btn->Enable();
	collection_mode->Enable();
	font_matcher->Enable();
	if (path.Decode("?script") == "?script")
		collection_mode->Enable(2, false);

	UpdateControls();
}

void DialogFontsCollector::OnIdle(wxIdleEvent&) {
	if (goto_end_on_idle) {
		goto_end_on_idle = false;
		collection_log->GotoPos(collection_log->GetLength());
	}
}
}

void ShowFontsCollectorDialog(agi::Context *c) {
	c->GetUI().dialog->Show<DialogFontsCollector>(c);
}
