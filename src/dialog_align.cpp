// Copyright (c) 2019, Charlie Jiang
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

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "dialog_manager.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "video_frame.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "async_video_provider.h"
#include "colour_button.h"
#include "image_position_picker.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>

#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

namespace {
	constexpr int kKeyPointCoarseScanStep = 8;

	class DialogAlignToVideo final : public wxDialog {
		agi::Context* context;
		AsyncVideoProvider* provider;

		wxImage preview_image;
		int current_n_frame;

		ImagePositionPicker* preview_frame;
		ColourButton* selected_color;
		wxTextCtrl* selected_x;
		wxTextCtrl* selected_y;
		wxTextCtrl* selected_tolerance;

		void update_from_textbox();
		void update_from_textbox(wxCommandEvent&);

		void process(wxEvent&);
	public:
		DialogAlignToVideo(agi::Context* context);
		~DialogAlignToVideo();
	};

	DialogAlignToVideo::DialogAlignToVideo(agi::Context* context)
		: wxDialog(context->parent, -1, _("Align subtitle to video by key point"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxMAXIMIZE_BOX | wxRESIZE_BORDER)
		, context(context), provider(context->project->VideoProvider())
	{
		auto add_with_label = [&](wxSizer * sizer, wxString const& label, wxWindow * ctrl) {
			sizer->Add(new wxStaticText(this, -1, label), 0, wxLEFT | wxRIGHT | wxCENTER, 3);
			sizer->Add(ctrl, 1, wxLEFT);
		};

		auto tolerance = OPT_GET("Tool/Align to Video/Tolerance")->GetInt();
		auto maximized = OPT_GET("Tool/Align to Video/Maximized")->GetBool();

		current_n_frame = context->videoController->GetFrameN();
		auto frame = provider->GetFrameBgra(
			current_n_frame,
			context->project->Timecodes().TimeAtFrame(current_n_frame),
			true);
		if (!frame || frame->data.empty())
			throw agi::InternalError("Could not retrieve a BGRA preview frame for key-point alignment.");
		preview_image = GetImage(*frame);

		preview_frame = new ImagePositionPicker(this, preview_image, [&](int x, int y, unsigned char r, unsigned char g, unsigned char b) -> void {
			selected_x->ChangeValue(wxString::Format(wxT("%i"), x));
			selected_y->ChangeValue(wxString::Format(wxT("%i"), y));

			selected_color->SetColor(agi::Color(r, g, b));
			});
		selected_color = new ColourButton(this, wxSize(55, 16), true, agi::Color("FFFFFF"));
		selected_color->SetToolTip(_("The key color to be followed"));
		selected_x = new wxTextCtrl(this, -1, "0");
		selected_x->SetToolTip(_("The x coord of the key point"));
		selected_y = new wxTextCtrl(this, -1, "0");
		selected_y->SetToolTip(_("The y coord of the key point"));
		selected_tolerance = new wxTextCtrl(this, -1, wxString::Format(wxT("%i"), int(tolerance)));
		selected_tolerance->SetToolTip(_("Max tolerance of the color"));

		selected_x->Bind(wxEVT_TEXT, &DialogAlignToVideo::update_from_textbox, this);
		selected_y->Bind(wxEVT_TEXT, &DialogAlignToVideo::update_from_textbox, this);
		update_from_textbox();

		wxFlexGridSizer* right_sizer = new wxFlexGridSizer(4, 2, 5, 5);
		add_with_label(right_sizer, _("X"), selected_x);
		add_with_label(right_sizer, _("Y"), selected_y);
		add_with_label(right_sizer, _("Color"), selected_color);
		add_with_label(right_sizer, _("Tolerance"), selected_tolerance);
		right_sizer->AddGrowableCol(1, 1);

		wxSizer* main_sizer = new wxBoxSizer(wxHORIZONTAL);

		main_sizer->Add(preview_frame, 1, (wxALL & ~wxRIGHT) | wxEXPAND, 5);
		main_sizer->Add(right_sizer, 0, wxALIGN_LEFT, 5);

		wxSizer* dialog_sizer = new wxBoxSizer(wxVERTICAL);
		dialog_sizer->Add(main_sizer, wxSizerFlags(1).Border(wxALL & ~wxBOTTOM).Expand());
		dialog_sizer->Add(CreateButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Right().Border());
		SetSizerAndFit(dialog_sizer);
		SetSize(1024, 700);
		CenterOnParent();

		Bind(wxEVT_BUTTON, &DialogAlignToVideo::process, this, wxID_OK);
		Bind(wxEVT_LEFT_DCLICK, &DialogAlignToVideo::process, this, preview_frame->GetId());
		SetIcon(GETICON(button_align_16));
		if (maximized)
			wxDialog::Maximize(true);
	}

	DialogAlignToVideo::~DialogAlignToVideo()
	{
		long lt;
		if (!selected_tolerance->GetValue().ToLong(&lt))
			return;
		if (lt < 0 || lt > 255)
			return;

		OPT_SET("Tool/Align to Video/Tolerance")->SetInt(lt);
	}

	void DialogAlignToVideo::process(wxEvent &)
	{
		auto w = provider->GetWidth();
		auto h = provider->GetHeight();

		long lx, ly, lt;
		if (!selected_x->GetValue().ToLong(&lx) || !selected_y->GetValue().ToLong(&ly) || !selected_tolerance->GetValue().ToLong(&lt))
		{
			wxMessageBox(_("Bad x or y position or tolerance value!"));
			return;
		}
		if (lx < 0 || ly < 0 || lx >= w || ly >= h)
		{
			wxMessageBox(wxString::Format(_("Bad x or y position! Require: 0 <= x < %i, 0 <= y < %i"), w, h));
			return;
		}
		if (lt < 0 || lt > 255)
		{
			wxMessageBox(_("Bad tolerance value! Require: 0 <= torlerance <= 255"));
			return;
		}
		int x = int(lx), y = int(ly);
		unsigned char tolerance = (unsigned char)(lt);

		auto color = selected_color->GetColor();
		auto r = color.r;
		auto b = color.b;
		auto g = color.g;
		auto scan = provider->FindKeyPointRange({
			current_n_frame,
			x,
			y,
			r,
			g,
			b,
			tolerance,
			kKeyPointCoarseScanStep,
			5
		});
		if (scan.status == KeyPointRangeScanStatus::FrameUnavailable) {
			wxMessageBox(_("Could not retrieve a CPU-readable frame for key-point alignment."));
			return;
		}
		if (scan.status == KeyPointRangeScanStatus::AnchorMismatch) {
			wxMessageBox(_("Selected position and color are not within tolerance!"));
			return;
		}
		if (scan.status != KeyPointRangeScanStatus::Success) {
			wxMessageBox(_("Could not scan the requested key point range."));
			return;
		}

		auto timecode = context->project->Timecodes();
		auto line = context->selectionController->GetActiveLine();
		if (!line) {
			wxMessageBox(_("No active subtitle line is selected."));
			return;
		}
		line->Start = timecode.TimeAtFrame(scan.left, agi::vfr::Time::START);
		line->End = timecode.TimeAtFrame(scan.right, agi::vfr::Time::END); // exclusive
		context->ass->Commit(_("Align to video by key point"), AssFile::COMMIT_DIAG_TIME);
		Close();
	}

	void DialogAlignToVideo::update_from_textbox()
	{
		long lx, ly;
		int w = preview_image.GetWidth(), h = preview_image.GetHeight();
		if (!selected_x->GetValue().ToLong(&lx) || !selected_y->GetValue().ToLong(&ly))
			return;

		if (lx < 0 || ly < 0 || lx >= w || ly >= h)
			return;
		int x = int(lx);
		int y = int(ly);
		auto r = preview_image.GetRed(x, y);
		auto g = preview_image.GetGreen(x, y);
		auto b = preview_image.GetBlue(x, y);
		selected_color->SetColor(agi::Color(r, g, b));
	}

	void DialogAlignToVideo::update_from_textbox(wxCommandEvent & evt)
	{
		update_from_textbox();
	}

}


void ShowAlignToVideoDialog(agi::Context * c)
{
	c->dialog->Show<DialogAlignToVideo>(c);
}
