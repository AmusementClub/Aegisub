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
#include "include/aegisub/context_ui.h"
#include "video_frame_wx.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "async_video_provider.h"
#include "align_video_fade.h"
#include "colour_button.h"
#include "image_position_picker.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>

#include <wx/dialog.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

#include <algorithm>

namespace {
	// Key-point alignment needs contiguous frame ranges, so do not skip over
	// short mismatch runs between two matching samples.
	constexpr int kKeyPointScanStep = 1;

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
		wxCheckBox* detect_fade;

		void update_from_textbox();
		void update_from_textbox(wxCommandEvent&);

		void process(wxEvent&);
	public:
		DialogAlignToVideo(agi::Context* context);
		~DialogAlignToVideo();
	};

	DialogAlignToVideo::DialogAlignToVideo(agi::Context* context)
		: wxDialog(context->GetUI().parent, -1, _("Align subtitle to video by key point"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxMAXIMIZE_BOX | wxRESIZE_BORDER)
		, context(context), provider(context->GetCore().project->VideoProvider())
	{
		auto core = context->GetCore();
		auto add_with_label = [&](wxSizer * sizer, wxString const& label, wxWindow * ctrl) {
			sizer->Add(new wxStaticText(this, -1, label), 0, wxLEFT | wxRIGHT | wxCENTER, 3);
			sizer->Add(ctrl, 1, wxLEFT);
		};

		auto tolerance = OPT_GET("Tool/Align to Video/Tolerance")->GetInt();
		auto maximized = OPT_GET("Tool/Align to Video/Maximized")->GetBool();
		auto detect_fade_option = OPT_GET("Tool/Align to Video/Detect Fade")->GetBool();

		current_n_frame = core.videoController->GetFrameN();
		auto frame = provider->GetFrameBgra(
			current_n_frame,
			core.project->Timecodes().TimeAtFrame(current_n_frame),
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
		selected_x = new wxTextCtrl(this, -1, wxS("0"));
		selected_x->SetToolTip(_("The x coord of the key point"));
		selected_y = new wxTextCtrl(this, -1, wxS("0"));
		selected_y->SetToolTip(_("The y coord of the key point"));
		selected_tolerance = new wxTextCtrl(this, -1, wxString::Format(wxT("%i"), int(tolerance)));
		selected_tolerance->SetToolTip(_("Max tolerance of the color"));
		detect_fade = new wxCheckBox(this, -1, _("Enable fade detection"));
		detect_fade->SetValue(detect_fade_option);
		detect_fade->SetToolTip(_("Extend the aligned range and write ASS fade tags when a fade is detected"));

		selected_x->Bind(wxEVT_TEXT, &DialogAlignToVideo::update_from_textbox, this);
		selected_y->Bind(wxEVT_TEXT, &DialogAlignToVideo::update_from_textbox, this);
		update_from_textbox();

		wxFlexGridSizer* right_sizer = new wxFlexGridSizer(0, 2, 5, 5);
		add_with_label(right_sizer, _("X"), selected_x);
		add_with_label(right_sizer, _("Y"), selected_y);
		add_with_label(right_sizer, _("Color"), selected_color);
		add_with_label(right_sizer, _("Tolerance"), selected_tolerance);
		add_with_label(right_sizer, _("Fade"), detect_fade);
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
		OPT_SET("Tool/Align to Video/Detect Fade")->SetBool(detect_fade->GetValue());

		long lt;
		if (!selected_tolerance->GetValue().ToLong(&lt))
			return;
		if (lt < 0 || lt > 255)
			return;

		OPT_SET("Tool/Align to Video/Tolerance")->SetInt(lt);
	}

	void DialogAlignToVideo::process(wxEvent &)
	{
		auto core = context->GetCore();
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
		auto timecode = core.project->Timecodes();
		bool const detect_fade_enabled = detect_fade->GetValue();
		int max_fade_frames = 0;
		if (detect_fade_enabled) {
			int const anchor_time = timecode.TimeAtFrame(current_n_frame, agi::vfr::EXACT);
			int const before = current_n_frame - timecode.FrameAtTime(anchor_time - 2000, agi::vfr::EXACT);
			int const after = timecode.FrameAtTime(anchor_time + 2000, agi::vfr::EXACT) - current_n_frame;
			max_fade_frames = std::clamp(std::max({ before, after, 1 }), 1, 240);
		}

		KeyPointRangeScanRequest request;
		request.frame = current_n_frame;
		request.x = x;
		request.y = y;
		request.r = r;
		request.g = g;
		request.b = b;
		request.tolerance = tolerance;
		request.scan_step = kKeyPointScanStep;
		request.bounds_tolerance = 5;
		request.detect_fade = detect_fade_enabled;
		request.max_fade_frames = max_fade_frames;
		auto scan = provider->FindKeyPointRange(request);
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

		auto line = core.selectionController->GetActiveLine();
		if (!line) {
			wxMessageBox(_("No active subtitle line is selected."));
			return;
		}
		auto const fade_timing = aegisub::align_video_fade::BuildAssFadeTiming(
			timecode,
			scan.left,
			scan.right,
			scan.fade_in_end,
			scan.fade_out_start,
			scan.fade_in_detected,
			scan.fade_out_detected);
		line->Start = fade_timing.start_ms;
		line->End = fade_timing.end_ms;

		bool text_changed = false;
		if (detect_fade_enabled && (scan.fade_in_detected || scan.fade_out_detected)) {
			auto const updated = aegisub::align_video_fade::ApplyAssFade(
				line->Text.get(),
				fade_timing.end_ms - fade_timing.start_ms,
				fade_timing.fade_in_ms,
				fade_timing.fade_out_ms);
			text_changed = updated.text != line->Text.get();
			if (text_changed)
				line->Text = updated.text;
		}

		int commit_flags = AssFile::COMMIT_DIAG_TIME;
		if (text_changed)
			commit_flags |= AssFile::COMMIT_DIAG_TEXT;
		core.ass->Commit(from_wx(_("Align to video by key point")), commit_flags);
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
	c->GetUI().dialog->Show<DialogAlignToVideo>(c);
}
