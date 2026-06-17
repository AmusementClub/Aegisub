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

#include "ass_file.h"
#include "compat.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "project.h"
#include "resample_dialog_policy.h"
#include "resolution_resampler.h"
#include "validators.h"

#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/valgen.h>

namespace {
/// @class DialogResample
/// @brief Configuration dialog for resolution resampling
///
/// Populate a ResampleSettings structure with data from the user
struct DialogResample {
	wxDialog d;
	agi::Context *c; ///< Project context

	ResampleDialogReference reference;

	wxSpinCtrl *source_x;
	wxSpinCtrl *source_y;
	wxSpinCtrl *dest_x;
	wxSpinCtrl *dest_y;
	wxComboBox *source_matrix;
	wxComboBox *dest_matrix;
	wxCheckBox *symmetrical;
	wxRadioBox *ar_mode;
	wxSpinCtrl *margin_ctrl[4];

	wxButton *from_script;
	wxButton *from_video;

	void SetSourceFromScript(wxCommandEvent &);
	/// Set the destination resolution to the video's resolution
	void SetDestFromVideo(wxCommandEvent &);
	/// Symmetrical checkbox toggle handler
	void OnSymmetrical(wxCommandEvent &);
	/// Copy margin values over if symmetrical is enabled
	void OnMarginChange(wxSpinCtrl *src, wxSpinCtrl *dst);
	void UpdateButtons();

public:
	/// Constructor
	/// @param context Project context
	/// @param[out] settings Settings struct to populate
	DialogResample(agi::Context *context, ResampleSettings &settings);
};

enum {
	LEFT = 0,
	RIGHT = 1,
	TOP = 2,
	BOTTOM = 3
};

DialogResample::DialogResample(agi::Context *c, ResampleSettings &settings)
: d(c->GetUI().parent, -1, _("Resample Resolution"))
, c(c)
{
	auto core = c->GetCore();
	d.SetIcon(GETICON(resample_toolbutton_16));

	reference = BuildResampleDialogReference(*core.ass, core.project->VideoProvider());
	InitializeResampleSettings(settings, reference);

	// Create all controls and set validators
	for (size_t i = 0; i < 4; ++i) {
		margin_ctrl[i] = new wxSpinCtrl(&d, -1, wxS("0"), wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, -9999, 9999, 0);
		margin_ctrl[i]->SetValidator(wxGenericValidator(&settings.margin[i]));
	}

	symmetrical = new wxCheckBox(&d, -1, _("&Symmetrical"));
	symmetrical->SetValue(true);

	margin_ctrl[RIGHT]->Enable(false);
	margin_ctrl[BOTTOM]->Enable(false);

	source_x = new wxSpinCtrl(&d, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, INT_MAX);
	source_y = new wxSpinCtrl(&d, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, INT_MAX);
	source_matrix = new wxComboBox(&d, -1, wxEmptyString, wxDefaultPosition,
		wxDefaultSize, to_wx(MatrixNames()), wxCB_READONLY);
	dest_x = new wxSpinCtrl(&d, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, INT_MAX);
	dest_y = new wxSpinCtrl(&d, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, INT_MAX);
	dest_matrix = new wxComboBox(&d, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		to_wx(MatrixNames()), wxCB_READONLY);

	source_x->SetValidator(wxGenericValidator(&settings.source_x));
	source_y->SetValidator(wxGenericValidator(&settings.source_y));
	source_matrix->SetValidator(MakeEnumBinder(&settings.source_matrix));
	dest_x->SetValidator(wxGenericValidator(&settings.dest_x));
	dest_y->SetValidator(wxGenericValidator(&settings.dest_y));
	dest_matrix->SetValidator(MakeEnumBinder(&settings.dest_matrix));

	from_video = new wxButton(&d, -1, _("From &video"));
	from_video->Enable(false);
	from_script = new wxButton(&d, -1, _("From s&cript"));
	from_script->Enable(false);

	wxString ar_modes[] = {_("Stretch"), _("Add borders"), _("Remove borders"), _("Manual")};
	ar_mode = new wxRadioBox(&d, -1, _("Aspect Ratio Handling"), wxDefaultPosition,
		wxDefaultSize, std::size(ar_modes), ar_modes, 1, 4, MakeEnumBinder(&settings.ar_mode));

	// Position the controls
	auto margin_sizer = new wxGridSizer(3, 3, 5, 5);
	margin_sizer->AddSpacer(1);
	margin_sizer->Add(margin_ctrl[TOP], wxSizerFlags(1).Expand());
	margin_sizer->AddSpacer(1);
	margin_sizer->Add(margin_ctrl[LEFT], wxSizerFlags(1).Expand());
	margin_sizer->Add(symmetrical, wxSizerFlags(1).Expand());
	margin_sizer->Add(margin_ctrl[RIGHT], wxSizerFlags(1).Expand());
	margin_sizer->AddSpacer(1);
	margin_sizer->Add(margin_ctrl[BOTTOM], wxSizerFlags(1).Expand());
	margin_sizer->AddSpacer(1);

	auto margin_box = new wxStaticBoxSizer(wxVERTICAL, &d, _("Margin offset"));
	margin_box->Add(margin_sizer, wxSizerFlags(1).Expand().Border(wxBOTTOM));

	auto source_res_sizer = new wxBoxSizer(wxHORIZONTAL);
	source_res_sizer->Add(source_x, wxSizerFlags(1).Border(wxRIGHT).Align(wxALIGN_CENTER_VERTICAL));
	source_res_sizer->Add(new wxStaticText(&d, -1, _("x")), wxSizerFlags().Center().Border(wxRIGHT));
	source_res_sizer->Add(source_y, wxSizerFlags(1).Border(wxRIGHT).Align(wxALIGN_CENTER_VERTICAL));
	source_res_sizer->Add(from_script, wxSizerFlags(1));

	auto source_matrix_sizer = new wxBoxSizer(wxHORIZONTAL);
	source_matrix_sizer->Add(new wxStaticText(&d, -1, _("YCbCr Matrix:")), wxSizerFlags().Border(wxRIGHT).Center());
	source_matrix_sizer->Add(source_matrix, wxSizerFlags(1).Center().Right());

	auto source_res_box = new wxStaticBoxSizer(wxVERTICAL, &d, _("Source Resolution"));
	source_res_box->Add(source_res_sizer, wxSizerFlags(1).Expand().Border(wxBOTTOM));
	source_res_box->Add(source_matrix_sizer, wxSizerFlags(1).Expand());

	auto dest_res_sizer = new wxBoxSizer(wxHORIZONTAL);
	dest_res_sizer->Add(dest_x, wxSizerFlags(1).Border(wxRIGHT).Align(wxALIGN_CENTER_VERTICAL));
	dest_res_sizer->Add(new wxStaticText(&d, -1, _("x")), wxSizerFlags().Center().Border(wxRIGHT));
	dest_res_sizer->Add(dest_y, wxSizerFlags(1).Border(wxRIGHT).Align(wxALIGN_CENTER_VERTICAL));
	dest_res_sizer->Add(from_video, wxSizerFlags(1));

	auto dest_matrix_sizer = new wxBoxSizer(wxHORIZONTAL);
	dest_matrix_sizer->Add(new wxStaticText(&d, -1, _("YCbCr Matrix:")), wxSizerFlags().Border(wxRIGHT).Center());
	dest_matrix_sizer->Add(dest_matrix, wxSizerFlags(1).Center().Right());

	auto dest_res_box = new wxStaticBoxSizer(wxVERTICAL, &d, _("Destination Resolution"));
	dest_res_box->Add(dest_res_sizer, wxSizerFlags(1).Expand().Border(wxBOTTOM));
	dest_res_box->Add(dest_matrix_sizer, wxSizerFlags(1).Expand());

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(source_res_box, wxSizerFlags().Expand().Border());
	main_sizer->Add(dest_res_box, wxSizerFlags().Expand().Border());
	main_sizer->Add(ar_mode, wxSizerFlags().Expand().Border());
	main_sizer->Add(margin_box, wxSizerFlags(1).Expand().Border());
	main_sizer->Add(d.CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxHELP), wxSizerFlags().Expand().Border(wxALL & ~wxTOP));
	d.SetSizerAndFit(main_sizer);
	d.CenterOnParent();

	d.TransferDataToWindow();
	UpdateButtons();

	// Bind events
	using std::bind;
	d.Bind(wxEVT_BUTTON, bind(&HelpButton::OpenPage, "Resample resolution"), wxID_HELP);
	d.Bind(wxEVT_SPINCTRL, [=](wxCommandEvent&) { UpdateButtons(); });
	d.Bind(wxEVT_RADIOBOX, [=](wxCommandEvent&) { UpdateButtons(); });
	from_video->Bind(wxEVT_BUTTON, &DialogResample::SetDestFromVideo, this);
	from_script->Bind(wxEVT_BUTTON, &DialogResample::SetSourceFromScript, this);
	symmetrical->Bind(wxEVT_CHECKBOX, &DialogResample::OnSymmetrical, this);
	margin_ctrl[LEFT]->Bind(wxEVT_SPINCTRL, bind(&DialogResample::OnMarginChange, this, margin_ctrl[LEFT], margin_ctrl[RIGHT]));
	margin_ctrl[TOP]->Bind(wxEVT_SPINCTRL, bind(&DialogResample::OnMarginChange, this, margin_ctrl[TOP], margin_ctrl[BOTTOM]));
}

void DialogResample::SetDestFromVideo(wxCommandEvent &) {
	dest_x->SetValue(reference.video_w);
	dest_y->SetValue(reference.video_h);
	dest_matrix->SetSelection((int)reference.video_matrix);
}

void DialogResample::SetSourceFromScript(wxCommandEvent&) {
	source_x->SetValue(reference.script_w);
	source_y->SetValue(reference.script_h);
	source_matrix->SetSelection((int)reference.script_matrix);
}

void DialogResample::UpdateButtons() {
	ResampleSettings current = {};
	current.source_x = source_x->GetValue();
	current.source_y = source_y->GetValue();
	current.dest_x = dest_x->GetValue();
	current.dest_y = dest_y->GetValue();
	current.ar_mode = static_cast<ResampleARMode>(ar_mode->GetSelection());

	auto state = BuildResampleDialogButtonState(reference, current, symmetrical->GetValue());
	from_video->Enable(state.from_video_enabled);
	from_script->Enable(state.from_script_enabled);
	ar_mode->Enable(state.ar_mode_enabled);
	symmetrical->Enable(state.symmetrical_enabled);
	margin_ctrl[LEFT]->Enable(state.margin_left_enabled);
	margin_ctrl[TOP]->Enable(state.margin_top_enabled);
	margin_ctrl[RIGHT]->Enable(state.margin_right_enabled);
	margin_ctrl[BOTTOM]->Enable(state.margin_bottom_enabled);
}

void DialogResample::OnSymmetrical(wxCommandEvent &) {
	bool state = !symmetrical->IsChecked();

	margin_ctrl[RIGHT]->Enable(state);
	margin_ctrl[BOTTOM]->Enable(state);

	if (!state) {
		margin_ctrl[RIGHT]->SetValue(margin_ctrl[LEFT]->GetValue());
		margin_ctrl[BOTTOM]->SetValue(margin_ctrl[TOP]->GetValue());
	}
}

void DialogResample::OnMarginChange(wxSpinCtrl *src, wxSpinCtrl *dst) {
	if (symmetrical->IsChecked())
		dst->SetValue(src->GetValue());
}

}

bool PromptForResampleSettings(agi::Context *c, ResampleSettings &settings) {
	if (HasLayoutResSensitiveTags(*c->GetCore().ass)) {
		auto result = wxMessageBox(
			_("This script contains \\frx, \\fry, or \\be tags whose rendering depends on the LayoutRes headers. Changing the resolution may alter their appearance in ways that cannot be automatically corrected.\n\nContinue anyway?"),
			_("LayoutRes-dependent tags detected"),
			wxYES_NO | wxICON_WARNING,
			c->GetUI().parent);
		if (result != wxYES)
			return false;
	}
	return DialogResample(c, settings).d.ShowModal() == wxID_OK;
}
