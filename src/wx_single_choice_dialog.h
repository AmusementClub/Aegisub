#pragma once

#include "compat.h"
#include "ui_dispatch.h"
#include "ui_services.h"

#include <algorithm>
#include <wx/dialog.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/window.h>

namespace agi {

// This header is the explicit wx adapter surface for single-choice
// interactions. Replace it when the GUI shell stops presenting wx dialogs
// for request/response selection.

inline std::optional<int> ShowSingleChoiceDialog(wxWindow *parent, SingleChoiceInteractionRequest const& request) {
	if (request.choices.empty())
		return std::nullopt;

	wxDialog dialog(parent, -1, to_wx(request.title));

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(new wxStaticText(&dialog, -1, to_wx(request.message)), wxSizerFlags().Border());

	auto choices = to_wx(request.choices);
	auto *radio_box = new wxRadioBox(&dialog, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, choices, 1);
	radio_box->SetSelection(std::clamp(request.default_choice, 0, static_cast<int>(request.choices.size() - 1)));
	sizer->Add(radio_box, wxSizerFlags().Border(wxALL & ~wxTOP).Expand());

	sizer->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Border().Expand());

	dialog.SetSizerAndFit(sizer);
	dialog.CenterOnParent();

	dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_OK); }, wxID_OK);
	dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); }, wxID_CANCEL);

	return dialog.ShowModal() == wxID_OK ? std::optional<int>(radio_box->GetSelection()) : std::nullopt;
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
