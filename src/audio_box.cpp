// Copyright (c) 2005, Rodrigo Braz Monteiro, Niels Martin Hansen
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

#include "audio_box.h"

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/toolbar.h"

#include "audio_controller.h"
#include "audio_display.h"
#include "audio_karaoke.h"
#include "audio_timing.h"
#include "options.h"
#include "project.h"
#include "toggle_bitmap.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/string.h>
#include <wx/menu.h>
#include <wx/toolbar.h>

namespace {
std::string GetSpectrumChannelLabel(int channel, int total_channels) {
	if (total_channels == 1)
		return "M";
	if (total_channels == 2)
		return channel == 0 ? "L" : "R";
	if (total_channels == 6) {
		static const char *labels[] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
		if (channel >= 0 && channel < 6)
			return labels[channel];
	}
	if (total_channels == 8) {
		static const char *labels[] = {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"};
		if (channel >= 0 && channel < 8)
			return labels[channel];
	}
	return "CH" + std::to_string(channel + 1);
}
}

enum {
	Audio_Horizontal_Zoom = 1600,
	Audio_Vertical_Zoom,
	Audio_Volume,
	Audio_SpectrumChannel,
};

AudioBox::AudioBox(wxWindow *parent, agi::Context *context)
: wxSashWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxSW_3D | wxCLIP_CHILDREN)
, controller(context->GetCore().audioController.get())
, context(context)
, audio_open_connection(context->GetCore().audioController->AddAudioPlayerOpenListener(&AudioBox::OnAudioOpen, this))
, panel(new wxPanel(this, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_RAISED))
, audioDisplay(new AudioDisplay(panel, context->GetCore().audioController.get(), context))
, HorizontalZoom(new wxSlider(panel, Audio_Horizontal_Zoom, -OPT_GET("Audio/Zoom/Horizontal")->GetInt(), -50, 30, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH))
, VerticalZoom(new wxSlider(panel, Audio_Vertical_Zoom, OPT_GET("Audio/Zoom/Vertical")->GetInt(), 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH|wxSL_INVERSE))
, VolumeBar(new wxSlider(panel, Audio_Volume, OPT_GET("Audio/Volume")->GetInt(), 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH|wxSL_INVERSE))
{
	SetSashVisible(wxSASH_BOTTOM, true);
	Bind(wxEVT_SASH_DRAGGED, &AudioBox::OnSashDrag, this);

	HorizontalZoom->SetToolTip(_("Horizontal zoom"));
	VerticalZoom->SetToolTip(_("Vertical zoom"));
	VolumeBar->SetToolTip(_("Audio Volume"));

	bool link = OPT_GET("Audio/Link")->GetBool();
	if (link) {
		VolumeBar->SetValue(VerticalZoom->GetValue());
		VolumeBar->Enable(false);
	}

	// VertVol sider
	wxSizer *VertVol = new wxBoxSizer(wxHORIZONTAL);
	VertVol->Add(VerticalZoom,1,wxEXPAND,0);
	VertVol->Add(VolumeBar,1,wxEXPAND,0);
	wxSizer *VertVolArea = new wxBoxSizer(wxVERTICAL);
	VertVolArea->Add(VertVol,1,wxEXPAND,0);

	auto link_btn = new ToggleBitmap(panel, context, "audio/opt/vertical_link", 16, "Audio", wxSize(20, -1));
	link_btn->SetMaxSize(wxDefaultSize);
	VertVolArea->Add(link_btn, 0, wxRIGHT | wxEXPAND, 0);
	OPT_SUB("Audio/Link", &AudioBox::OnVerticalLink, this);

	spectrum_channel_btn = new wxButton(panel, Audio_SpectrumChannel, _("CH"), wxDefaultPosition, wxSize(20, -1), wxBU_EXACTFIT);
	spectrum_channel_btn->SetToolTip(_("Spectrum display options"));
	spectrum_channel_btn->Enable(OPT_GET("Audio/Spectrum")->GetBool());
	VertVolArea->Add(spectrum_channel_btn, 0, wxEXPAND, 0);
	OPT_SUB("Audio/Spectrum", &AudioBox::OnSpectrumModeChange, this);

	// Top sizer
	wxSizer *TopSizer = new wxBoxSizer(wxHORIZONTAL);
	TopSizer->Add(audioDisplay,1,wxEXPAND,0);
	TopSizer->Add(HorizontalZoom,0,wxEXPAND,0);
	TopSizer->Add(VertVolArea,0,wxEXPAND,0);

	context->GetUI().karaoke = new AudioKaraoke(panel, context);

	// Main sizer
	auto MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(TopSizer,1,wxEXPAND|wxALL,3);
	MainSizer->Add(toolbar::GetToolbar(panel, "audio", context, "Audio"),0,wxEXPAND|wxLEFT|wxRIGHT,3);
	MainSizer->Add(context->GetUI().karaoke,0,wxEXPAND|wxALL,3);
	MainSizer->Show(context->GetUI().karaoke, false);
	panel->SetSizer(MainSizer);

	wxSizer *audioSashSizer = new wxBoxSizer(wxHORIZONTAL);
	audioSashSizer->Add(panel, 1, wxEXPAND);
	SetSizerAndFit(audioSashSizer);
	SetMinSize(wxSize(-1, OPT_GET("Audio/Display Height")->GetInt()));
	SetMinimumSizeY(panel->GetSize().GetHeight());

	audioDisplay->Bind(wxEVT_MOUSEWHEEL, &AudioBox::OnMouseWheel, this);
	zoom_preview_timer.Bind(wxEVT_TIMER, &AudioBox::OnZoomPreviewTimer, this);
	spectrum_prefetch_resume_timer.Bind(wxEVT_TIMER, &AudioBox::OnSpectrumPrefetchResumeTimer, this);

	audioDisplay->SetZoomLevel(-HorizontalZoom->GetValue());
	audioDisplay->SetAmplitudeScale(pow(mid(1, VerticalZoom->GetValue(), 100) / 50.0, 3));
}

void AudioBox::SyncToContextState() {
	context->GetUI().karaoke->SyncToContextState();
	audioDisplay->SyncToCurrentAudioProvider();

	if (context->GetCore().project->AudioProvider())
		OnAudioOpen();
}

BEGIN_EVENT_TABLE(AudioBox,wxSashWindow)
	EVT_COMMAND_SCROLL(Audio_Horizontal_Zoom, AudioBox::OnHorizontalZoom)
	EVT_COMMAND_SCROLL(Audio_Vertical_Zoom, AudioBox::OnVerticalZoom)
	EVT_COMMAND_SCROLL(Audio_Volume, AudioBox::OnVolume)
	EVT_BUTTON(Audio_SpectrumChannel, AudioBox::OnSpectrumChannelBtn)
END_EVENT_TABLE()

void AudioBox::OnMouseWheel(wxMouseEvent &evt) {
	if (!ForwardMouseWheelEvent(audioDisplay, evt))
		return;
	bool zoom = evt.CmdDown() != OPT_GET("Audio/Wheel Default to Zoom")->GetBool();
	if (!zoom) {
		int amount = -evt.GetWheelRotation();
		// If the user did a horizontal scroll the amount should be inverted
		// for it to be natural.
		if (evt.GetWheelAxis() == 1) amount = -amount;

		// Reset any accumulated zoom
		mouse_zoom_accum = 0;

		audioDisplay->ScrollBy(amount);
	}
	else if (evt.GetWheelAxis() == 0) {
		mouse_zoom_accum += evt.GetWheelRotation();
		int zoom_delta = mouse_zoom_accum / evt.GetWheelDelta();
		mouse_zoom_accum %= evt.GetWheelDelta();
		FlushPendingZoomPreview();
		SetHorizontalZoom(audioDisplay->GetZoomLevel() + zoom_delta);
	}
}

void AudioBox::OnSashDrag(wxSashEvent &event) {
	if (event.GetDragStatus() == wxSASH_STATUS_OUT_OF_RANGE)
		return;

	int new_height = std::min(event.GetDragRect().GetHeight(), GetParent()->GetSize().GetHeight() - 1);

	SetMinSize(wxSize(-1, new_height));
	GetParent()->Layout();

	// Karaoke mode is always disabled when the audio box is first opened, so
	// the initial height shouldn't include it
	auto karaoke = context->GetUI().karaoke;
	if (karaoke->IsEnabled())
		new_height -= karaoke->GetSize().GetHeight() + 6;

	OPT_SET("Audio/Display Height")->SetInt(new_height);
}

void AudioBox::OnHorizontalZoom(wxScrollEvent &event) {
	// Negate the value since we want zoom out to be on bottom and zoom in on top,
	// but the control doesn't want negative on bottom and positive on top.
	int new_zoom = -event.GetPosition();
	DisableSpectrumPrefetchTemporarily();
	auto event_type = event.GetEventType();
	if (event_type == wxEVT_SCROLL_THUMBTRACK) {
		pending_horizontal_zoom = new_zoom;
		horizontal_zoom_pending = true;
		if (!zoom_preview_timer.IsRunning())
			zoom_preview_timer.Start(GetZoomPreviewIntervalMs(false), true);
	}
	else if (event_type == wxEVT_SCROLL_THUMBRELEASE || event_type == wxEVT_SCROLL_CHANGED) {
		pending_horizontal_zoom = new_zoom;
		horizontal_zoom_pending = true;
		FlushPendingZoomPreview();
	}
	else {
		FlushPendingZoomPreview();
		SetHorizontalZoom(new_zoom);
	}
}

int AudioBox::GetZoomPreviewIntervalMs(bool vertical) const {
	if (vertical && OPT_GET("Audio/Spectrum")->GetBool())
		return spectrum_vertical_zoom_preview_interval_ms;
	return zoom_preview_interval_ms;
}

void AudioBox::DisableSpectrumPrefetchTemporarily() {
	if (!OPT_GET("Audio/Spectrum")->GetBool())
		return;
	if (!spectrum_prefetch_temporarily_disabled) {
		audioDisplay->SetInteractivePrefetchEnabled(false);
		spectrum_prefetch_temporarily_disabled = true;
	}
	if (spectrum_prefetch_resume_timer.IsRunning())
		spectrum_prefetch_resume_timer.Stop();
	spectrum_prefetch_resume_timer.Start(spectrum_prefetch_resume_delay_ms, true);
}

void AudioBox::SetHorizontalZoom(int new_zoom) {
	audioDisplay->SetZoomLevel(new_zoom);
	HorizontalZoom->SetValue(-new_zoom);
	OPT_SET("Audio/Zoom/Horizontal")->SetInt(new_zoom);
}

void AudioBox::ApplyVerticalZoomPos(int pos) {
	OPT_SET("Audio/Zoom/Vertical")->SetInt(pos);
	audioDisplay->SetAmplitudeScale(pow(pos / 50.0, 3));
	if (!VolumeBar->IsEnabled()) {
		VolumeBar->SetValue(pos);
		controller->SetVolume(pow(pos / 50.0, 3));
	}
}

void AudioBox::FlushPendingZoomPreview() {
	if (zoom_preview_timer.IsRunning())
		zoom_preview_timer.Stop();

	if (horizontal_zoom_pending) {
		SetHorizontalZoom(pending_horizontal_zoom);
		horizontal_zoom_pending = false;
	}

	if (vertical_zoom_pending) {
		ApplyVerticalZoomPos(pending_vertical_zoom_pos);
		vertical_zoom_pending = false;
	}
}

void AudioBox::OnZoomPreviewTimer(wxTimerEvent &) {
	FlushPendingZoomPreview();
}

void AudioBox::OnSpectrumPrefetchResumeTimer(wxTimerEvent &) {
	if (spectrum_prefetch_temporarily_disabled) {
		audioDisplay->SetInteractivePrefetchEnabled(true);
		spectrum_prefetch_temporarily_disabled = false;
	}
}

void AudioBox::OnVerticalZoom(wxScrollEvent &event) {
	int pos = mid(1, event.GetPosition(), 100);
	double value = pow(pos / 50.0, 3);
	DisableSpectrumPrefetchTemporarily();
	if (!VolumeBar->IsEnabled()) {
		VolumeBar->SetValue(pos);
		controller->SetVolume(value);
	}

	auto event_type = event.GetEventType();
	if (event_type == wxEVT_SCROLL_THUMBTRACK) {
		pending_vertical_zoom_pos = pos;
		vertical_zoom_pending = true;
		if (!zoom_preview_timer.IsRunning())
			zoom_preview_timer.Start(GetZoomPreviewIntervalMs(true), true);
	}
	else if (event_type == wxEVT_SCROLL_THUMBRELEASE || event_type == wxEVT_SCROLL_CHANGED) {
		pending_vertical_zoom_pos = pos;
		vertical_zoom_pending = true;
		FlushPendingZoomPreview();
	}
	else {
		FlushPendingZoomPreview();
		ApplyVerticalZoomPos(pos);
	}
}

void AudioBox::OnVolume(wxScrollEvent &event) {
	int pos = mid(1, event.GetPosition(), 100);
	OPT_SET("Audio/Volume")->SetInt(pos);
	controller->SetVolume(pow(pos / 50.0, 3));
}

void AudioBox::OnVerticalLink(agi::OptionValue const& opt) {
	if (opt.GetBool()) {
		int pos = mid(1, VerticalZoom->GetValue(), 100);
		double value = pow(pos / 50.0, 3);
		controller->SetVolume(value);
		VolumeBar->SetValue(pos);
	}
	VolumeBar->Enable(!opt.GetBool());
}

void AudioBox::OnAudioOpen() {
	controller->SetVolume(pow(mid(1, VolumeBar->GetValue(), 100) / 50.0, 3));
	audioDisplay->SetInteractivePrefetchEnabled(true);
	spectrum_prefetch_temporarily_disabled = false;
	if (spectrum_channel_btn)
		spectrum_channel_btn->Enable(OPT_GET("Audio/Spectrum")->GetBool());
}

void AudioBox::OnSpectrumModeChange(agi::OptionValue const& opt) {
	if (spectrum_channel_btn)
		spectrum_channel_btn->Enable(opt.GetBool());
}

void AudioBox::OnSpectrumChannelBtn(wxCommandEvent &) {
	const auto current_mode = audioDisplay->GetSpectrumChannelMode();
	const auto current_mono_mode = audioDisplay->GetSpectrumMonoMixMode();
	const int current_computation_mode = mid<int>(0, OPT_GET("Audio/Renderer/Spectrum/Computation Mode")->GetInt(), 1);
	const int current_freq_curve = mid<int>(0, OPT_GET("Audio/Renderer/Spectrum/FreqCurve")->GetInt(), 4);
	const int channels = std::max(1, audioDisplay->GetProviderChannels());
	std::vector<int> selected = audioDisplay->GetSpectrumSelectedChannels();
	if (selected.empty()) {
		selected.reserve(channels);
		for (int ch = 0; ch < channels; ++ch)
			selected.push_back(ch);
	}

	enum {
		ID_MONO = wxID_HIGHEST + 2000,
		ID_SPLIT,
		ID_MONO_AVG,
		ID_MONO_BIN_MAX,
		ID_MONO_BIN_AVG,
		ID_COMP_LEGACY,
		ID_COMP_CURVE,
		ID_CURVE_LINEAR,
		ID_CURVE_EXTENDED,
		ID_CURVE_MEDIUM,
		ID_CURVE_COMPRESSED,
		ID_CURVE_LOG,
		ID_CH_BASE = wxID_HIGHEST + 2100
	};
	wxMenu menu;
	menu.AppendRadioItem(ID_MONO,  _("Mono mix"))->Check(current_mode == AudioSpectrumChannelMode::MonoMix);
	menu.AppendRadioItem(ID_SPLIT, _("Split channels"))->Check(current_mode == AudioSpectrumChannelMode::ChannelSplit);

	wxMenu *mono_menu = new wxMenu();
	mono_menu->AppendRadioItem(ID_MONO_AVG, _("Time-domain downmix"))->Check(current_mono_mode == AudioSpectrumMonoMixMode::MonoAverage);
	mono_menu->AppendRadioItem(ID_MONO_BIN_MAX, _("Strongest channel per frequency bin"))->Check(current_mono_mode == AudioSpectrumMonoMixMode::PerBinMaxPower);
	mono_menu->AppendRadioItem(ID_MONO_BIN_AVG, _("Average channel energy per frequency bin"))->Check(current_mono_mode == AudioSpectrumMonoMixMode::PerBinAveragePower);
	menu.AppendSubMenu(mono_menu, _("Mono mix method"));

	wxMenu *computation_menu = new wxMenu();
	computation_menu->AppendRadioItem(ID_COMP_LEGACY, _("Legacy linear"))->Check(current_computation_mode == 0);
	computation_menu->AppendRadioItem(ID_COMP_CURVE, _("Frequency curve"))->Check(current_computation_mode == 1);
	menu.AppendSubMenu(computation_menu, _("Spectrum computation mode"));

	wxMenu *curve_menu = new wxMenu();
	curve_menu->AppendRadioItem(ID_CURVE_LINEAR, _("Linear"))->Check(current_freq_curve == 0);
	curve_menu->AppendRadioItem(ID_CURVE_EXTENDED, _("Extended"))->Check(current_freq_curve == 1);
	curve_menu->AppendRadioItem(ID_CURVE_MEDIUM, _("Medium"))->Check(current_freq_curve == 2);
	curve_menu->AppendRadioItem(ID_CURVE_COMPRESSED, _("Compressed"))->Check(current_freq_curve == 3);
	curve_menu->AppendRadioItem(ID_CURVE_LOG, _("Logarithmic"))->Check(current_freq_curve == 4);
	auto *curve_menu_item = menu.AppendSubMenu(curve_menu, _("Spectrum frequency mapping"));
	curve_menu_item->Enable(current_computation_mode == 1);

	wxMenu *split_menu = new wxMenu();
	for (int ch = 0; ch < channels; ++ch) {
		const int id = ID_CH_BASE + ch;
		const auto label = wxString::FromUTF8(GetSpectrumChannelLabel(ch, channels));
		const bool checked = std::find(selected.begin(), selected.end(), ch) != selected.end();
		split_menu->AppendCheckItem(id, label)->Check(checked);
	}
	menu.AppendSubMenu(split_menu, _("Split: visible channels"));

	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		audioDisplay->SetSpectrumChannelMode(AudioSpectrumChannelMode::MonoMix);
	}, ID_MONO);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		audioDisplay->SetSpectrumChannelMode(AudioSpectrumChannelMode::ChannelSplit);
	}, ID_SPLIT);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		audioDisplay->SetSpectrumChannelMode(AudioSpectrumChannelMode::MonoMix);
		OPT_SET("Audio/Renderer/Spectrum/Mono Mix Mode")->SetInt(static_cast<int>(AudioSpectrumMonoMixMode::MonoAverage));
	}, ID_MONO_AVG);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		audioDisplay->SetSpectrumChannelMode(AudioSpectrumChannelMode::MonoMix);
		OPT_SET("Audio/Renderer/Spectrum/Mono Mix Mode")->SetInt(static_cast<int>(AudioSpectrumMonoMixMode::PerBinMaxPower));
	}, ID_MONO_BIN_MAX);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		audioDisplay->SetSpectrumChannelMode(AudioSpectrumChannelMode::MonoMix);
		OPT_SET("Audio/Renderer/Spectrum/Mono Mix Mode")->SetInt(static_cast<int>(AudioSpectrumMonoMixMode::PerBinAveragePower));
	}, ID_MONO_BIN_AVG);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/Computation Mode")->SetInt(0);
	}, ID_COMP_LEGACY);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/Computation Mode")->SetInt(1);
	}, ID_COMP_CURVE);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(0);
	}, ID_CURVE_LINEAR);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(1);
	}, ID_CURVE_EXTENDED);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(2);
	}, ID_CURVE_MEDIUM);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(3);
	}, ID_CURVE_COMPRESSED);
	menu.Bind(wxEVT_MENU, [](wxCommandEvent &) {
		OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(4);
	}, ID_CURVE_LOG);

	for (int ch = 0; ch < channels; ++ch) {
		const int id = ID_CH_BASE + ch;
		menu.Bind(wxEVT_MENU, [this, ch, channels](wxCommandEvent &e) {
			audioDisplay->SetSpectrumChannelMode(AudioSpectrumChannelMode::ChannelSplit);
			auto cur = audioDisplay->GetSpectrumSelectedChannels();
			if (cur.empty()) {
				cur.reserve(channels);
				for (int i = 0; i < channels; ++i)
					cur.push_back(i);
			}
			auto it = std::find(cur.begin(), cur.end(), ch);
			if (e.IsChecked()) {
				if (it == cur.end())
					cur.push_back(ch);
			}
			else if (it != cur.end()) {
				cur.erase(it);
			}
			if (cur.empty())
				cur.push_back(ch);
			std::sort(cur.begin(), cur.end());
			cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
			audioDisplay->SetSpectrumSelectedChannels(cur);
		}, id);
	}
	PopupMenu(&menu);
}

void AudioBox::ShowKaraokeBar(bool show) {
	wxSizer *panel_sizer = panel->GetSizer();
	auto karaoke = context->GetUI().karaoke;
	if (panel_sizer->IsShown(karaoke) == show) return;

	int new_height = GetSize().GetHeight();
	int kara_height = karaoke->GetSize().GetHeight() + 6;

	if (show)
		new_height += kara_height;
	else
		new_height -= kara_height;

	panel_sizer->Show(karaoke, show);
	SetMinSize(wxSize(-1, new_height));
	GetParent()->Layout();
}

void AudioBox::ScrollAudioBy(int pixel_amount) {
	audioDisplay->ScrollBy(pixel_amount);
}

void AudioBox::ScrollToActiveLine() {
	if (controller->GetTimingController())
		audioDisplay->ScrollTimeRangeInView(controller->GetTimingController()->GetIdealVisibleTimeRange());
}
