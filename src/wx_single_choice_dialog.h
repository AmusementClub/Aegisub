#pragma once

#include "compat.h"
#include "single_choice_layout.h"
#include "ui_dispatch.h"
#include "ui_services.h"
#include "utils.h"

#include <algorithm>
#include <functional>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/translation.h>
#include <wx/utils.h>
#include <wx/window.h>

namespace agi {
namespace {

inline std::string LocalizeSubtitleFpsChoiceLabel(std::string const& choice) {
	if (choice == "15.000 FPS")
		return from_wx(_("15.000 FPS"));
	if (choice == "23.976 FPS (Decimated NTSC)")
		return from_wx(_("23.976 FPS (Decimated NTSC)"));
	if (choice == "24.000 FPS (FILM)")
		return from_wx(_("24.000 FPS (FILM)"));
	if (choice == "25.000 FPS (PAL)")
		return from_wx(_("25.000 FPS (PAL)"));
	if (choice == "29.970 FPS (NTSC)")
		return from_wx(_("29.970 FPS (NTSC)"));
	if (choice == "29.970 FPS (NTSC with SMPTE dropframe)")
		return from_wx(_("29.970 FPS (NTSC with SMPTE dropframe)"));
	if (choice == "30.000 FPS")
		return from_wx(_("30.000 FPS"));
	if (choice == "50.000 FPS (PAL x2)")
		return from_wx(_("50.000 FPS (PAL x2)"));
	if (choice == "59.940 FPS (NTSC x2)")
		return from_wx(_("59.940 FPS (NTSC x2)"));
	if (choice == "60.000 FPS")
		return from_wx(_("60.000 FPS"));
	if (choice == "119.880 FPS (NTSC x4)")
		return from_wx(_("119.880 FPS (NTSC x4)"));
	if (choice == "120.000 FPS")
		return from_wx(_("120.000 FPS"));
	if (choice == "From video (VFR)")
		return from_wx(_("From video (VFR)"));

	std::string const prefix = "From video (";
	if (choice.size() > prefix.size() + 1
		&& choice.compare(0, prefix.size(), prefix) == 0
		&& choice.back() == ')') {
		return choice;
	}

	return choice;
}

inline std::string LocalizeLocaleChoiceLabel(std::string const& choice) {
	if (choice.empty())
		return choice;
	return from_wx(LocalizedLanguageName(to_wx(choice)));
}

inline SingleChoiceInteractionRequest LocalizeKnownSingleChoiceRequest(SingleChoiceInteractionRequest request) {
	if (request.request_id == "charset_choice.detected_charsets") {
		request.title = from_wx(_("Choose character set"));
		request.message = from_wx(_("Aegisub could not narrow down the character set to a single one.\nPlease pick one below:"));
		return request;
	}

	if (request.request_id == "track_choice.audio") {
		request.title = from_wx(_("Choose audio track"));
		request.message = from_wx(_("Multiple audio tracks detected, please choose the one you wish to load:"));
		return request;
	}

	if (request.request_id == "track_choice.subtitle") {
		request.title = from_wx(_("Multiple subtitle tracks found"));
		request.message = from_wx(_("Choose which track to read:"));
		return request;
	}

	if (request.request_id == "track_choice.subtitle.secondary") {
		request.title = from_wx(_("Load embedded subtitles into the secondary subtitle strip"));
		request.message = from_wx(_("Choose which embedded subtitle track to load into the secondary subtitle strip:"));
		return request;
	}

	if (request.request_id == "track_choice.video") {
		request.title = from_wx(_("Choose video track"));
		request.message = from_wx(_("Multiple video tracks detected, please choose the one you wish to load:"));
		return request;
	}

	if (request.request_id == "subtitle_fps_choice.selection") {
		request.title = from_wx(_("FPS"));
		request.message = from_wx(_("Please choose the appropriate FPS for the subtitles:"));
		std::transform(request.choices.begin(), request.choices.end(), request.choices.begin(), [](std::string const& choice) {
			return LocalizeSubtitleFpsChoiceLabel(choice);
		});
		return request;
	}

	if (request.request_id == "locale_choice.ui_language") {
		request.title = from_wx(_("Language"));
		request.message = from_wx(_("Please choose a language:"));
		std::transform(request.choices.begin(), request.choices.end(), request.choices.begin(), [](std::string const& choice) {
			return LocalizeLocaleChoiceLabel(choice);
		});
	}

	return request;
}

}

// This header is the explicit wx adapter surface for single-choice
// interactions. Replace it when the GUI shell stops presenting wx dialogs
// for request/response selection.

inline std::optional<int> ShowSingleChoiceDialog(wxWindow *parent, SingleChoiceInteractionRequest const& request) {
	auto localized = LocalizeKnownSingleChoiceRequest(request);
	if (localized.choices.empty())
		return std::nullopt;

	auto const layout = aegisub::single_choice_layout::PlanLayout(localized.choices.size());
	bool const scrollable =
		layout.presentation == aegisub::single_choice_layout::Presentation::ScrollableList;

	long style = wxDEFAULT_DIALOG_STYLE;
	if (scrollable)
		style |= wxRESIZE_BORDER;

	wxDialog dialog(parent, -1, to_wx(localized.title), wxDefaultPosition, wxDefaultSize, style);

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(new wxStaticText(&dialog, -1, to_wx(localized.message)), wxSizerFlags().Border());

	auto choices = to_wx(localized.choices);
	int const default_sel = std::clamp(
		localized.default_choice, 0, static_cast<int>(localized.choices.size() - 1));

	// Unified selection reader so ShowModal only samples once after dismiss.
	std::function<int()> get_selection;
	wxListBox *scroll_target = nullptr;

	if (!scrollable) {
		auto *radio_box = new wxRadioBox(
			&dialog, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, choices, 1);
		radio_box->SetSelection(default_sel);
		sizer->Add(radio_box, wxSizerFlags().Border(wxALL & ~wxTOP).Expand());
		get_selection = [radio_box] { return radio_box->GetSelection(); };
	} else {
		// wxLB_NEEDED_SB is 0 in wx 3.3 (documentary no-op); omit it. Vertical
		// scrollbars appear as needed by default; HSCROLL covers long track labels.
		auto *list_box = new wxListBox(
			&dialog, -1, wxDefaultPosition, wxDefaultSize, choices,
			wxLB_SINGLE | wxLB_HSCROLL);
		list_box->SetSelection(default_sel);
		scroll_target = list_box;

		// Prefer a short, readable window; long track labels scroll horizontally.
		int const row_h = list_box->GetCharHeight() + list_box->FromDIP(2);
		int const min_h = layout.visible_rows * row_h + list_box->FromDIP(8);
		auto best = list_box->GetBestSize();
		int const min_w = list_box->FromDIP(360);
		int const max_w = list_box->FromDIP(720);
		int const width = std::clamp(best.GetWidth(), min_w, max_w);
		list_box->SetMinSize(wxSize(width, min_h));

		// Expand with the dialog when the user resizes.
		sizer->Add(list_box, wxSizerFlags(1).Border(wxALL & ~wxTOP).Expand());

		list_box->Bind(wxEVT_LISTBOX_DCLICK, [&dialog, list_box](wxCommandEvent&) {
			if (list_box->GetSelection() != wxNOT_FOUND)
				dialog.EndModal(wxID_OK);
		});
		get_selection = [list_box] { return list_box->GetSelection(); };
	}

	sizer->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Border().Expand());

	// List boxes (unlike radio boxes) can clear the selection; keep OK enabled
	// only while a real index is selected so Enter/OK cannot confirm wxNOT_FOUND.
	auto sync_ok_enabled = [&dialog, get_selection] {
		if (auto *ok = dialog.FindWindow(wxID_OK))
			ok->Enable(get_selection() != wxNOT_FOUND);
	};
	if (scroll_target) {
		scroll_target->Bind(wxEVT_LISTBOX, [sync_ok_enabled](wxCommandEvent&) {
			sync_ok_enabled();
		});
		sync_ok_enabled();
	}

	dialog.SetSizerAndFit(sizer);
	if (scrollable)
		dialog.SetMinSize(dialog.GetSize());
	dialog.CenterOnParent();

	if (scroll_target) {
		// Scroll after layout: before SetSizerAndFit the list is still at its
		// default size, so EnsureVisible would compute against the wrong height.
		// EnsureVisible asserts on an invalid index, and default_sel is only in
		// range because of the empty-choices early return above; re-check here so
		// relaxing that guard cannot turn into a debug-build assert.
		if (default_sel >= 0 && static_cast<size_t>(default_sel) < localized.choices.size())
			scroll_target->EnsureVisible(default_sel);
		scroll_target->SetFocus();
	}

	dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_OK); }, wxID_OK);
	dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); }, wxID_CANCEL);

	if (dialog.ShowModal() != wxID_OK)
		return std::nullopt;

	// Defense in depth: list boxes can report wxNOT_FOUND, and a non-wx sink
	// might still hand back a bare invalid index. Empty/out-of-range is cancel.
	// AskForFPS also range-checks before ResolveSubtitleFpsChoiceSelection.
	int const selection = get_selection();
	if (selection < 0 || static_cast<size_t>(selection) >= localized.choices.size())
		return std::nullopt;
	return selection;
}

class WxSingleChoiceInteractionSink final : public SingleChoiceInteractionSink {
	wxWindow *parent = nullptr;

public:
	explicit WxSingleChoiceInteractionSink(wxWindow *parent = nullptr)
	: parent(parent) {
	}

	std::optional<int> RequestSingleChoice(SingleChoiceInteractionRequest const& request) override {
		return agi::ui::MainInvoke([parent = parent, request] {
			bool was_busy = wxIsBusy();
			if (was_busy)
				wxEndBusyCursor();

			auto result = ShowSingleChoiceDialog(parent, request);

			if (was_busy)
				wxBeginBusyCursor();
			return result;
		});
	}
};

inline std::shared_ptr<SingleChoiceInteractionSink> MakeWindowSingleChoiceInteractionSink(wxWindow *parent = nullptr) {
	return std::make_shared<WxSingleChoiceInteractionSink>(parent);
}

}
