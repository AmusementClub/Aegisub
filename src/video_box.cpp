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

#include "video_box.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/toolbar.h"
#include "options.h"
#include "project.h"
#include "secondary_subtitle_strip.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_slider.h"

#include <wx/combobox.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/toolbar.h>

VideoBox::VideoBox(wxWindow *parent, bool isDetached, agi::Context *context)
: wxPanel(parent, -1)
, context(context)
, current_frame(context->GetCore().videoController->GetFrameN())
{
	auto videoSlider = new VideoSlider(this, context);
	videoSlider->SetToolTip(_("Seek video"));

	auto mainToolbar = toolbar::GetToolbar(this, "video", context, "Video", false);

	VideoPosition = new wxTextCtrl(this, -1, wxEmptyString, wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoPosition->SetToolTip(_("Current frame time and number"));

	VideoSubsPos = new wxTextCtrl(this, -1, wxEmptyString, wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoSubsPos->SetToolTip(_("Time of this frame relative to start and end of current subs"));

	wxArrayString choices;
	for (int i = 1; i <= 24; ++i)
		choices.Add(fmt_wx("%g%%", i * 12.5));
	auto zoomBox = new wxComboBox(this, -1, wxS("75%"), wxDefaultPosition, wxDefaultSize, choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);

	auto visualToolBar = toolbar::GetToolbar(this, "visual_tools", context, "Video", true);
	auto visualSubToolBar = new wxToolBar(this, -1, wxDefaultPosition, wxDefaultSize, wxTB_VERTICAL | wxTB_BOTTOM | wxTB_NODIVIDER | wxTB_FLAT);

	videoDisplay = new VideoDisplay(visualSubToolBar, isDetached, zoomBox, this, context);
	videoDisplay->MoveBeforeInTabOrder(videoSlider);

	auto toolbarSizer = new wxBoxSizer(wxVERTICAL);
	toolbarSizer->Add(visualToolBar, wxSizerFlags(1));
	toolbarSizer->Add(visualSubToolBar, wxSizerFlags());

	auto topSizer = new wxBoxSizer(wxHORIZONTAL);
	topSizer->Add(toolbarSizer, 0, wxEXPAND);
	topSizer->Add(videoDisplay, isDetached, isDetached ? wxEXPAND : 0);

	auto videoBottomSizer = new wxBoxSizer(wxHORIZONTAL);
	videoBottomSizer->Add(mainToolbar, wxSizerFlags(0).Center());
	videoBottomSizer->Add(VideoPosition, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(VideoSubsPos, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(zoomBox, wxSizerFlags(0).Center().Border(wxLEFT | wxRIGHT));

	auto VideoSizer = new wxBoxSizer(wxVERTICAL);
	VideoSizer->Add(topSizer, 1, wxEXPAND, 0);
	VideoSizer->Add(new wxStaticLine(this), 0, wxEXPAND, 0);
	VideoSizer->Add(videoSlider, 0, wxEXPAND, 0);
	VideoSizer->Add(videoBottomSizer, 0, wxEXPAND | wxBOTTOM, 5);
	if (!isDetached) {
		secondarySubtitleStripSeparator = new wxStaticLine(this);
		secondarySubtitleStrip = new SecondarySubtitleStrip(this, context);
		VideoSizer->Add(secondarySubtitleStripSeparator, 0, wxEXPAND, 0);
		VideoSizer->Add(secondarySubtitleStrip, 0, wxEXPAND, 0);
		VideoSizer->Show(secondarySubtitleStripSeparator, false);
		VideoSizer->Show(secondarySubtitleStrip, false);
		secondarySubtitleStrip->SetSessionActive(false);
	}
	SetSizer(VideoSizer);
	Bind(wxEVT_SIZE, &VideoBox::OnSize, this);

	UpdateTimeBoxes();
	UpdateSecondarySubtitleStripVisibility();

	auto core = context->GetCore();
	connections = agi::signal::make_vector({
		core.ass->AddCommitListener(&VideoBox::UpdateTimeBoxes, this),
		core.project->AddKeyframesListener(&VideoBox::UpdateTimeBoxes, this),
		core.project->AddTimecodesListener(&VideoBox::UpdateTimeBoxes, this),
		core.project->AddVideoProviderListener(&VideoBox::OnVideoProviderChanged, this),
		core.selectionController->AddSelectionListener(&VideoBox::UpdateTimeBoxes, this),
		core.videoController->AddFramePresentedListener(&VideoBox::OnCurrentFrameChanged, this),
		OPT_SUB("Video/Detached/Enabled", &VideoBox::OnDetachedVideoChanged, this),
		OPT_SUB("Video/Secondary Subtitles/Enabled", &VideoBox::OnSecondarySubtitleStripEnabledChanged, this),
	});
}

void VideoBox::SyncToContextState() {
	ApplyVideoProvider();
	if (auto video_display = context->GetUI().videoDisplay)
		video_display->SyncToCurrentVideoProvider();
}

void VideoBox::ApplyVideoProvider() {
	auto core = context->GetCore();
	current_frame = core.project->VideoProvider() ? core.videoController->GetFrameN() : -1;
	UpdateTimeBoxes();
}

bool VideoBox::OpenSecondarySubtitlesFromPath(agi::fs::path const& path, bool show_errors) {
	return secondarySubtitleStrip && secondarySubtitleStrip->OpenExternalSubtitlesFromPath(path, show_errors);
}

void VideoBox::UpdateTimeBoxes() {
	auto core = context->GetCore();
	if (!core.project->VideoProvider()) return;

	int frame = current_frame >= 0 ? current_frame : core.videoController->GetFrameN();
	int time = core.videoController->TimeAtFrame(frame, agi::vfr::EXACT);

	// Set the text box for frame number and time
	VideoPosition->SetValue(fmt_wx("%s - %d", agi::Time(time).GetAssFormatted(true), frame));
	if (std::binary_search(core.project->Keyframes().begin(), core.project->Keyframes().end(), frame)) {
		// Set the background color to indicate this is a keyframe
		VideoPosition->SetBackgroundColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
		VideoPosition->SetForegroundColour(to_wx(OPT_GET("Colour/Subtitle Grid/Selection")->GetColor()));
	}
	else {
		VideoPosition->SetBackgroundColour(wxNullColour);
		VideoPosition->SetForegroundColour(wxNullColour);
	}

	AssDialogue *active_line = core.selectionController->GetActiveLine();
	if (!active_line)
		VideoSubsPos->SetValue(wxString());
	else {
		VideoSubsPos->SetValue(fmt_wx(
			"%+dms; %+dms; %dms",
			time - active_line->Start,
			time - active_line->End, active_line->End - active_line->Start));
	}

	VideoPosition->Refresh(false);
	VideoPosition->Update();
	VideoSubsPos->Refresh(false);
	VideoSubsPos->Update();
}

void VideoBox::OnCurrentFrameChanged(int frame_number) {
	current_frame = frame_number;
	UpdateTimeBoxes();
}

void VideoBox::UpdateSecondarySubtitleStripVisibility() {
	if (!secondarySubtitleStrip || !secondarySubtitleStripSeparator || !GetSizer())
		return;

	auto core = context->GetCore();
	bool const show_strip =
		static_cast<bool>(core.project->VideoProvider())
		&& OPT_GET("Video/Secondary Subtitles/Enabled")->GetBool()
		&& !OPT_GET("Video/Detached/Enabled")->GetBool();
	bool const visibility_changed = secondarySubtitleStrip->IsShown() != show_strip;
	int const preserved_video_height = videoDisplay ? videoDisplay->GetClientSize().GetHeight() : 0;
	int const previous_min_height = GetSecondarySubtitleLayoutMinHeight();

	GetSizer()->Show(secondarySubtitleStripSeparator, show_strip);
	GetSizer()->Show(secondarySubtitleStrip, show_strip);
	secondarySubtitleStrip->SetSessionActive(show_strip);
	if (visibility_changed) {
		int const new_min_height = GetSecondarySubtitleLayoutMinHeight();
		RelayoutAfterSecondarySubtitleStripChange(
			preserved_video_height,
			new_min_height - previous_min_height);
	}
	else {
		GetSizer()->Layout();
		Layout();
		UpdateSecondarySubtitleStripGutter();
	}
}

void VideoBox::UpdateSecondarySubtitleStripGutter() {
	if (!secondarySubtitleStrip || !videoDisplay)
		return;

	secondarySubtitleStrip->SetLeftGutterWidth(std::max(videoDisplay->GetPosition().x, 0));
}

int VideoBox::GetSecondarySubtitleLayoutMinHeight() const {
	auto *sizer = const_cast<VideoBox *>(this)->GetSizer();
	return sizer ? sizer->CalcMin().GetHeight() : 0;
}

void VideoBox::RelayoutAfterSecondarySubtitleStripChange(int preserved_video_height, int preferred_client_height_delta) {
	if (!videoDisplay)
		return;

	for (wxWindow *window = this; window; window = window->GetParent())
		window->InvalidateBestSize();

	auto relayout = [this] {
		if (GetSizer())
			GetSizer()->Layout();
		Layout();
		UpdateSecondarySubtitleStripGutter();
		if (auto *parent = GetParent())
			parent->Layout();
	};

	auto *top = dynamic_cast<wxTopLevelWindow *>(wxGetTopLevelParent(this));
	bool const can_resize_top = top && !top->IsMaximized() && !top->IsFullScreen();
	auto resize_top_client = [top, can_resize_top](int delta_height) {
		if (!can_resize_top || delta_height == 0)
			return;

		wxSize target_client_size = top->GetClientSize() + wxSize(0, delta_height);
		target_client_size.SetHeight(std::max(target_client_size.GetHeight(), 1));
		top->SetClientSize(target_client_size);
		top->SendSizeEvent(0);
	};

	resize_top_client(preferred_client_height_delta);
	relayout();

	if (preserved_video_height > 0) {
		int const video_height_delta = preserved_video_height - videoDisplay->GetClientSize().GetHeight();
		resize_top_client(video_height_delta);
	}
	relayout();

	Refresh();
	Update();
	if (auto *parent = GetParent()) {
		parent->Refresh();
		parent->Update();
	}
}

void VideoBox::OnVideoProviderChanged() {
	ApplyVideoProvider();
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::OnDetachedVideoChanged(agi::OptionValue const&) {
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::OnSecondarySubtitleStripEnabledChanged(agi::OptionValue const&) {
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::OnSecondarySubtitleStripHeightChanged(int previous_height, int new_height) {
	if (!secondarySubtitleStrip || !secondarySubtitleStrip->IsShown() || !videoDisplay)
		return;

	if (previous_height == new_height)
		return;

	if (secondarySubtitleStripHeightDragActive) {
		PreviewSecondarySubtitleStripHeightChange();
		return;
	}

	int const preserved_video_height = videoDisplay->GetClientSize().GetHeight();
	RelayoutAfterSecondarySubtitleStripChange(preserved_video_height, new_height - previous_height);
}

void VideoBox::BeginSecondarySubtitleStripHeightDrag() {
	if (secondarySubtitleStripHeightDragActive)
		return;

	secondarySubtitleStripHeightDragActive = true;
	secondarySubtitleStripHeightDragPreservedVideoHeight =
		videoDisplay ? videoDisplay->GetClientSize().GetHeight() : 0;
}

void VideoBox::PreviewSecondarySubtitleStripHeightChange() {
	for (wxWindow *window = this; window; window = window->GetParent())
		window->InvalidateBestSize();

	if (GetSizer())
		GetSizer()->Layout();
	Layout();
	UpdateSecondarySubtitleStripGutter();
	if (auto *parent = GetParent()) {
		parent->Layout();
	}
	if (secondarySubtitleStripSeparator && secondarySubtitleStripSeparator->IsShown())
		secondarySubtitleStripSeparator->Refresh(false);
	if (secondarySubtitleStrip) {
		secondarySubtitleStrip->Refresh(false);
		secondarySubtitleStrip->Update();
	}
	if (videoDisplay) {
		videoDisplay->Refresh(false);
		videoDisplay->Update();
	}
}

void VideoBox::CommitSecondarySubtitleStripHeightDrag() {
	if (!secondarySubtitleStripHeightDragActive)
		return;

	secondarySubtitleStripHeightDragActive = false;
	RelayoutAfterSecondarySubtitleStripChange(
		secondarySubtitleStripHeightDragPreservedVideoHeight,
		0);
	secondarySubtitleStripHeightDragPreservedVideoHeight = 0;
}
void VideoBox::OnSize(wxSizeEvent &event) {
	event.Skip();
	if (!secondarySubtitleStrip)
		return;

	CallAfter([this] {
		if (secondarySubtitleStrip)
			UpdateSecondarySubtitleStripGutter();
	});
}
