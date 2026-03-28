// Copyright (c) 2005, Rodrigo Braz Monteiro
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

#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "project.h"
#include "resolution_resampler.h"
#include "validators.h"

#include <libaegisub/string_utils.h>
#include <vector>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
class DialogProperties {
	wxDialog d;
	agi::Context *c; ///< Project this dialog is adjusting the properties of

	/// Pairs of a script property and a text control for that property
	std::vector<std::pair<std::string, wxTextCtrl*>> properties;

	// Things that effect rendering
	wxComboBox *WrapStyle;   ///< Wrapping style for long lines
	wxTextCtrl *PlayResX;
	wxTextCtrl *PlayResY;
	wxTextCtrl *LayoutResX;
	wxTextCtrl *LayoutResY;
	wxStaticText *EffectiveResolution;
	wxCheckBox *ScaleBorder; ///< If script resolution != video resolution how should borders be handled
	wxComboBox *YCbCrMatrix;

	/// OK button handler
	void OnOK(wxCommandEvent &event);
	void OnSetPlayResFromVideo(wxCommandEvent &event);
	void OnSetLayoutResFromVideo(wxCommandEvent &event);
	/// Set a script info field
	/// @param key Name of field
	/// @param value New value
	/// @return Did the value actually need to be changed?
	int SetInfoIfDifferent(std::string const& key, std::string const& value);

	/// Add a property with label and text box for updating the property
	/// @param sizer Sizer to add the label and control to
	/// @param label Label text to use
	/// @param property Script info property name
	void AddProperty(wxSizer *sizer, wxString const& label, std::string const& property);

public:
	/// Constructor
	/// @param c Project context
	DialogProperties(agi::Context *c);
	wxString GetEffectiveResolutionText() const;
	void ShowModal() { d.ShowModal(); }
};

DialogProperties::DialogProperties(agi::Context *c)
: d(c->GetUI().parent, -1, _("Script Properties"))
, c(c)
{
	auto core = c->GetCore();
	d.SetIcon(GETICON(properties_toolbutton_16));

	// Button sizer
	// Create buttons first. See:
	//  https://github.com/wangqr/Aegisub/issues/6
	//  https://trac.wxwidgets.org/ticket/18472#comment:9
	auto ButtonSizer = d.CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxHELP);
	d.Bind(wxEVT_BUTTON, &DialogProperties::OnOK, this, wxID_OK);
	d.Bind(wxEVT_BUTTON, std::bind(&HelpButton::OpenPage, "Properties"), wxID_HELP);

	// Script details crap
	wxSizer *TopSizer = new wxStaticBoxSizer(wxHORIZONTAL,&d,_("Script"));
	auto TopSizerGrid = new wxFlexGridSizer(0,2,5,5);

	AddProperty(TopSizerGrid, _("Title:"), "Title");
	AddProperty(TopSizerGrid, _("Original script:"), "Original Script");
	AddProperty(TopSizerGrid, _("Translation:"), "Original Translation");
	AddProperty(TopSizerGrid, _("Editing:"), "Original Editing");
	AddProperty(TopSizerGrid, _("Timing:"), "Original Timing");
	AddProperty(TopSizerGrid, _("Synch point:"), "Synch Point");
	AddProperty(TopSizerGrid, _("Updated by:"), "Script Updated By");
	AddProperty(TopSizerGrid, _("Update details:"), "Update Details");

	TopSizerGrid->AddGrowableCol(1,1);
	TopSizer->Add(TopSizerGrid,1,wxALL | wxEXPAND,0);

	// Resolution box
	PlayResX = new wxTextCtrl(&d,-1,wxEmptyString,wxDefaultPosition,wxDefaultSize,0,IntValidator(core.ass->GetScriptInfoAsInt("PlayResX")));
	PlayResY = new wxTextCtrl(&d,-1,wxEmptyString,wxDefaultPosition,wxDefaultSize,0,IntValidator(core.ass->GetScriptInfoAsInt("PlayResY")));
	LayoutResX = new wxTextCtrl(&d,-1,wxEmptyString,wxDefaultPosition,wxDefaultSize,0,IntValidator(core.ass->GetScriptInfoAsInt("LayoutResX")));
	LayoutResY = new wxTextCtrl(&d,-1,wxEmptyString,wxDefaultPosition,wxDefaultSize,0,IntValidator(core.ass->GetScriptInfoAsInt("LayoutResY")));

	wxButton *PlayResFromVideo = new wxButton(&d,-1,_("From &video"));
	wxButton *LayoutResFromVideo = new wxButton(&d,-1,_("From v&ideo"));
	if (!core.project->VideoProvider()) {
		PlayResFromVideo->Enable(false);
		LayoutResFromVideo->Enable(false);
	}
	else {
		PlayResFromVideo->Bind(wxEVT_BUTTON, &DialogProperties::OnSetPlayResFromVideo, this);
		LayoutResFromVideo->Bind(wxEVT_BUTTON, &DialogProperties::OnSetLayoutResFromVideo, this);
	}

	auto resolution_grid = new wxFlexGridSizer(2, 5, 5, 5);
	resolution_grid->Add(new wxStaticText(&d, -1, wxS("PlayRes:")), 0, wxALIGN_CENTER_VERTICAL);
	resolution_grid->Add(PlayResX, 1, wxEXPAND);
	resolution_grid->Add(new wxStaticText(&d, -1, wxS("x")), 0, wxALIGN_CENTER);
	resolution_grid->Add(PlayResY, 1, wxEXPAND);
	resolution_grid->Add(PlayResFromVideo, 0, wxEXPAND);
	resolution_grid->Add(new wxStaticText(&d, -1, wxS("LayoutRes:")), 0, wxALIGN_CENTER_VERTICAL);
	resolution_grid->Add(LayoutResX, 1, wxEXPAND);
	resolution_grid->Add(new wxStaticText(&d, -1, wxS("x")), 0, wxALIGN_CENTER);
	resolution_grid->Add(LayoutResY, 1, wxEXPAND);
	resolution_grid->Add(LayoutResFromVideo, 0, wxEXPAND);
	resolution_grid->AddGrowableCol(1, 1);
	resolution_grid->AddGrowableCol(3, 1);

	EffectiveResolution = new wxStaticText(&d, -1, GetEffectiveResolutionText());

	YCbCrMatrix = new wxComboBox(&d, -1, to_wx(core.ass->GetScriptInfo("YCbCr Matrix")),
		 wxDefaultPosition, wxDefaultSize, to_wx(MatrixNames()), wxCB_READONLY);

	auto matrix_sizer = new wxBoxSizer(wxHORIZONTAL);
	matrix_sizer->Add(new wxStaticText(&d, -1, wxS("YCbCr Matrix:")), wxSizerFlags().Center());
	matrix_sizer->Add(YCbCrMatrix, wxSizerFlags(1).Expand().Border(wxLEFT));

	auto res_box = new wxStaticBoxSizer(wxVERTICAL, &d, _("Resolution"));
	res_box->Add(resolution_grid, wxSizerFlags().Expand());
	res_box->Add(EffectiveResolution, wxSizerFlags().Border(wxTOP).Expand());
	res_box->Add(matrix_sizer, wxSizerFlags().Border(wxTOP).Expand());

	// Options
	wxSizer *optionsBox = new wxStaticBoxSizer(wxHORIZONTAL,&d,_("Options"));
	auto optionsGrid = new wxFlexGridSizer(3,2,5,5);
	wxString wrap_opts[] = {
		_("0: Smart wrapping, top line is wider"),
		_("1: End-of-line word wrapping, only \\N breaks"),
		_("2: No word wrapping, both \\n and \\N break"),
		_("3: Smart wrapping, bottom line is wider")
	};
	WrapStyle = new wxComboBox(&d, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, 4, wrap_opts, wxCB_READONLY);
	WrapStyle->SetSelection(core.ass->GetScriptInfoAsInt("WrapStyle"));
	optionsGrid->Add(new wxStaticText(&d,-1,_("Wrap Style: ")),0,wxALIGN_CENTER_VERTICAL,0);
	optionsGrid->Add(WrapStyle,1,wxEXPAND,0);

	ScaleBorder = new wxCheckBox(&d,-1,_("Scale Border and Shadow"));
	ScaleBorder->SetToolTip(_("Scale border and shadow together with script/render resolution. If this is unchecked, relative border and shadow size will depend on renderer."));
	ScaleBorder->SetValue(agi::util::strings::iequals(core.ass->GetScriptInfo("ScaledBorderAndShadow"), "yes"));
	optionsGrid->AddSpacer(0);
	optionsGrid->Add(ScaleBorder,1,wxEXPAND,0);
	optionsGrid->AddGrowableCol(1,1);
	optionsBox->Add(optionsGrid,1,wxEXPAND,0);

	// MainSizer
	wxSizer *MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(TopSizer,0,wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND,5);
	MainSizer->Add(res_box,0,wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND,5);
	MainSizer->Add(optionsBox,0,wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND,5);
	MainSizer->Add(ButtonSizer,0,wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND,5);

	d.SetSizerAndFit(MainSizer);
	d.CenterOnParent();
}

void DialogProperties::AddProperty(wxSizer *sizer, wxString const& label, std::string const& property) {
	wxTextCtrl *ctrl = new wxTextCtrl(&d, -1, to_wx(c->GetCore().ass->GetScriptInfo(property)));
	sizer->Add(new wxStaticText(&d, -1, label), wxSizerFlags().Center().Left());
	sizer->Add(ctrl, wxSizerFlags(1).Expand());
	properties.push_back({property, ctrl});
}

void DialogProperties::OnOK(wxCommandEvent &) {
	int count = 0;
	for (auto const& prop : properties)
		count += SetInfoIfDifferent(prop.first, from_wx(prop.second->GetValue()));

	count += SetInfoIfDifferent("PlayResX", from_wx(PlayResX->GetValue()));
	count += SetInfoIfDifferent("PlayResY", from_wx(PlayResY->GetValue()));
	count += SetInfoIfDifferent("LayoutResX", from_wx(LayoutResX->GetValue()));
	count += SetInfoIfDifferent("LayoutResY", from_wx(LayoutResY->GetValue()));
	count += SetInfoIfDifferent("WrapStyle", std::to_string(WrapStyle->GetSelection()));
	count += SetInfoIfDifferent("ScaledBorderAndShadow", ScaleBorder->GetValue() ? "yes" : "no");
	count += SetInfoIfDifferent("YCbCr Matrix", from_wx(YCbCrMatrix->GetValue()));

	if (count)
		c->GetCore().ass->Commit(from_wx(_("property changes")), AssFile::COMMIT_SCRIPTINFO);

	d.EndModal(!!count);
}

int DialogProperties::SetInfoIfDifferent(std::string const& key, std::string const&value) {
	auto core = c->GetCore();
	if (core.ass->GetScriptInfo(key) != value) {
		core.ass->SetScriptInfo(key, value);
		return 1;
	}
	return 0;
}

wxString DialogProperties::GetEffectiveResolutionText() const {
	int width, height;
	auto type = c->GetCore().ass->GetResolutionType(width, height);
	wxString source = wxS("Fallback");
	if (type == ScriptResolutionType::PlayRes)
		source = wxS("PlayRes");
	else if (type == ScriptResolutionType::LayoutRes)
		source = wxS("LayoutRes");
	return wxString::Format(_("Current effective resolution: %s %d x %d"), source, width, height);
}

void DialogProperties::OnSetPlayResFromVideo(wxCommandEvent &) {
	auto provider = c->GetCore().project->VideoProvider();
	PlayResX->SetValue(std::to_wstring(provider->GetWidth()));
	PlayResY->SetValue(std::to_wstring(provider->GetHeight()));
}

void DialogProperties::OnSetLayoutResFromVideo(wxCommandEvent &) {
	auto provider = c->GetCore().project->VideoProvider();
	LayoutResX->SetValue(std::to_wstring(provider->GetWidth()));
	LayoutResY->SetValue(std::to_wstring(provider->GetHeight()));
}
}

void ShowPropertiesDialog(agi::Context *c) {
	DialogProperties(c).ShowModal();
}
