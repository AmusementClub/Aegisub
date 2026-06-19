// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "secondary_subtitle_strip.h"

#include "compat.h"
#include "colour_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/subtitles_provider.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "secondary_subtitle_session.h"
#include "secondary_subtitle_strip_layout.h"
#include "utils.h"
#include "video_box.h"

#include <libaegisub/make_unique.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/menu.h>
#include <wx/scrolbar.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/toolbar.h>

namespace {
enum SecondarySubtitleStripMenuId {
	MenuUseCurrentScript = wxID_HIGHEST + 1200,
	MenuOpenExternalSubtitles,
	MenuConfigureSecondarySubtitleStrip,
	MenuSecondarySubtitleStripOptionsButton,
	MenuSecondarySubtitleStripReloadButton,
	MenuFollowPreferencesSubtitlesProvider,
	MenuSecondarySubtitleProviderFirst = wxID_HIGHEST + 1300
};

int GetScrollBarWidth(wxWindow *window) {
	int width = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, window);
	return std::max(width, window->FromDIP(12));
}

int GetGutterButtonExtent(wxWindow *window, wxToolBar *toolbar) {
	wxSize best_size = toolbar->GetBestSize();
	return std::max({window->FromDIP(20), best_size.GetWidth(), best_size.GetHeight()});
}

int PickNiceRulerStep(double target_step) {
	if (target_step <= 1.0)
		return 1;

	double exponent = std::floor(std::log10(target_step));
	double scale = std::pow(10.0, exponent);
	double normalized = target_step / scale;
	double nice = 10.0;
	if (normalized <= 1.0)
		nice = 1.0;
	else if (normalized <= 2.0)
		nice = 2.0;
	else if (normalized <= 5.0)
		nice = 5.0;
	return std::max(static_cast<int>(std::lround(nice * scale)), 1);
}

class SecondarySubtitleStripSettingsDialog final : public wxDialog {
	wxSpinCtrl *height = nullptr;
	ColourButton *dummy_background = nullptr;
	wxCheckBox *dummy_checkerboard = nullptr;
	wxCheckBox *show_vertical_ruler = nullptr;
	wxButton *apply = nullptr;
	int applied_height = 0;
	agi::Color applied_dummy_background;
	bool applied_dummy_checkerboard = false;
	bool applied_show_vertical_ruler = false;

	void MarkDirty() {
		if (apply)
			apply->Enable(
				height->GetValue() != applied_height ||
				dummy_background->GetColor() != applied_dummy_background ||
				dummy_checkerboard->GetValue() != applied_dummy_checkerboard ||
				show_vertical_ruler->GetValue() != applied_show_vertical_ruler);
	}

	void ApplyChanges() {
		int new_height = height->GetValue();
		auto new_dummy_background = dummy_background->GetColor();
		bool new_dummy_checkerboard = dummy_checkerboard->GetValue();
		bool new_show_vertical_ruler = show_vertical_ruler->GetValue();
		if (new_height != applied_height) {
			OPT_SET("Video/Secondary Subtitles/Height")->SetInt(new_height);
			applied_height = new_height;
		}
		if (new_dummy_background != applied_dummy_background) {
			OPT_SET("Colour/Secondary Subtitle Strip/Dummy Background")->SetColor(new_dummy_background);
			applied_dummy_background = new_dummy_background;
		}
		if (new_dummy_checkerboard != applied_dummy_checkerboard) {
			OPT_SET("Video/Secondary Subtitles/Dummy/Pattern")->SetBool(new_dummy_checkerboard);
			applied_dummy_checkerboard = new_dummy_checkerboard;
		}
		if (new_show_vertical_ruler != applied_show_vertical_ruler) {
			OPT_SET("Video/Secondary Subtitles/Show Vertical Ruler")->SetBool(new_show_vertical_ruler);
			applied_show_vertical_ruler = new_show_vertical_ruler;
		}
		MarkDirty();
	}

public:
	SecondarySubtitleStripSettingsDialog(wxWindow *parent, int min_height, int max_height)
	: wxDialog(parent, wxID_ANY, _("Secondary Subtitle Strip Settings"))
	, applied_height(std::clamp(
		static_cast<int>(OPT_GET("Video/Secondary Subtitles/Height")->GetInt()),
		min_height,
		max_height))
	, applied_dummy_background(OPT_GET("Colour/Secondary Subtitle Strip/Dummy Background")->GetColor())
	, applied_dummy_checkerboard(OPT_GET("Video/Secondary Subtitles/Dummy/Pattern")->GetBool())
	, applied_show_vertical_ruler(OPT_GET("Video/Secondary Subtitles/Show Vertical Ruler")->GetBool()) {
		auto *height_label = new wxStaticText(this, wxID_ANY, _("Strip height"));
		height = new wxSpinCtrl(
			this,
			wxID_ANY,
			wxEmptyString,
			wxDefaultPosition,
			wxDefaultSize,
			wxSP_ARROW_KEYS,
			min_height,
			max_height,
			applied_height);
		auto *height_unit = new wxStaticText(this, wxID_ANY, _("DIP"));

		auto *dummy_background_label = new wxStaticText(this, wxID_ANY, _("Dummy background"));
		dummy_background = new ColourButton(this, wxSize(42, 16), false, applied_dummy_background);
		auto *dummy_checkerboard_label = new wxStaticText(this, wxID_ANY, _("Dummy pattern"));
		dummy_checkerboard = new wxCheckBox(this, wxID_ANY, _("Checkerboard pattern"));
		dummy_checkerboard->SetValue(applied_dummy_checkerboard);

		auto *show_vertical_ruler_label = new wxStaticText(this, wxID_ANY, _("Vertical ruler"));
		show_vertical_ruler = new wxCheckBox(this, wxID_ANY, _("Show coordinates"));
		show_vertical_ruler->SetValue(applied_show_vertical_ruler);

		auto *grid = new wxFlexGridSizer(4, 3, FromDIP(6), FromDIP(8));
		grid->Add(height_label, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->Add(height, wxSizerFlags().Expand());
		grid->Add(height_unit, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->Add(dummy_background_label, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->Add(dummy_background, wxSizerFlags());
		grid->AddSpacer(1);
		grid->Add(dummy_checkerboard_label, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->Add(dummy_checkerboard, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->AddSpacer(1);
		grid->Add(show_vertical_ruler_label, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->Add(show_vertical_ruler, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		grid->AddSpacer(1);
		grid->AddGrowableCol(1);

		auto *button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxAPPLY);
		apply = button_sizer->GetApplyButton();
		if (apply)
			apply->Enable(false);

		auto *main_sizer = new wxBoxSizer(wxVERTICAL);
		main_sizer->Add(grid, wxSizerFlags().Expand().Border(wxALL, FromDIP(8)));
		main_sizer->Add(button_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8)));
		SetSizerAndFit(main_sizer);
		CenterOnParent();

		height->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { MarkDirty(); });
		height->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { MarkDirty(); });
		dummy_background->Bind(EVT_COLOR, [this](ValueEvent<agi::Color>&) { MarkDirty(); });
		dummy_checkerboard->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { MarkDirty(); });
		show_vertical_ruler->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { MarkDirty(); });
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ApplyChanges(); }, wxID_APPLY);
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ApplyChanges();
			EndModal(wxID_OK);
		}, wxID_OK);
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); }, wxID_CANCEL);
	}
};

void ShowSecondarySubtitleStripSettings(wxWindow *parent, int min_height, int max_height) {
	SecondarySubtitleStripSettingsDialog(parent, min_height, max_height).ShowModal();
}
}

SecondarySubtitleStrip::SecondarySubtitleStrip(wxWindow *parent, agi::Context *context)
: wxPanel(parent, -1)
, session(agi::make_unique<SecondarySubtitleSession>(context))
, entry_button(new wxToolBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTB_FLAT | wxTB_NODIVIDER | wxTB_HORIZONTAL))
, reload_button(new wxToolBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTB_FLAT | wxTB_NODIVIDER | wxTB_HORIZONTAL))
, scroll_bar(new wxScrollBar(this, -1, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL)) {
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	ApplyConfiguredHeight(static_cast<int>(OPT_GET("Video/Secondary Subtitles/Height")->GetInt()));
	scroll_offset_y = std::max(
		static_cast<int>(OPT_GET("Video/Secondary Subtitles/Scroll Offset Y")->GetInt()),
		0);

	session->SetBitmapUpdatedCallback([this] {
		UpdateScrollBar();
		Refresh(false);
	});

	height_option_connection = OPT_SUB("Video/Secondary Subtitles/Height", &SecondarySubtitleStrip::OnConfiguredHeightChanged, this);
	scroll_offset_option_connection = OPT_SUB(
		"Video/Secondary Subtitles/Scroll Offset Y",
		&SecondarySubtitleStrip::OnConfiguredScrollOffsetChanged,
		this);
	toolbar_icon_size_option_connection = OPT_SUB(
		"App/Toolbar Icon Size",
		&SecondarySubtitleStrip::OnToolbarIconSizeChanged,
		this);
	video_dpi_scale_option_connection = OPT_SUB(
		"Video/Scale with DPI",
		&SecondarySubtitleStrip::OnVideoDpiScaleChanged,
		this);
	show_vertical_ruler_option_connection = OPT_SUB(
		"Video/Secondary Subtitles/Show Vertical Ruler",
		&SecondarySubtitleStrip::OnConfiguredShowVerticalRulerChanged,
		this);
	RefreshGutterToolbars();
	entry_button->Bind(wxEVT_TOOL, &SecondarySubtitleStrip::OnEntryButton, this, MenuSecondarySubtitleStripOptionsButton);
	entry_button->SetToolTip(_("Secondary subtitle strip options"));
	reload_button->Bind(wxEVT_TOOL, &SecondarySubtitleStrip::OnReloadButton, this, MenuSecondarySubtitleStripReloadButton);
	reload_button->SetToolTip(_("Reload secondary source"));

	auto bind_scroll = [this](wxEventTypeTag<wxScrollEvent> event_type) {
		scroll_bar->Bind(event_type, &SecondarySubtitleStrip::OnScroll, this);
	};
	bind_scroll(wxEVT_SCROLL_TOP);
	bind_scroll(wxEVT_SCROLL_BOTTOM);
	bind_scroll(wxEVT_SCROLL_LINEUP);
	bind_scroll(wxEVT_SCROLL_LINEDOWN);
	bind_scroll(wxEVT_SCROLL_PAGEUP);
	bind_scroll(wxEVT_SCROLL_PAGEDOWN);
	bind_scroll(wxEVT_SCROLL_THUMBTRACK);
	bind_scroll(wxEVT_SCROLL_THUMBRELEASE);
	bind_scroll(wxEVT_SCROLL_CHANGED);

	Bind(wxEVT_PAINT, &SecondarySubtitleStrip::OnPaint, this);
	Bind(wxEVT_SIZE, &SecondarySubtitleStrip::OnSize, this);
	Bind(wxEVT_LEFT_DOWN, &SecondarySubtitleStrip::OnMouseEvent, this);
	Bind(wxEVT_LEFT_UP, &SecondarySubtitleStrip::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_DOWN, &SecondarySubtitleStrip::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_UP, &SecondarySubtitleStrip::OnMouseEvent, this);
	Bind(wxEVT_MOTION, &SecondarySubtitleStrip::OnMouseEvent, this);
	Bind(wxEVT_LEAVE_WINDOW, &SecondarySubtitleStrip::OnMouseEvent, this);
	Bind(wxEVT_MOUSE_CAPTURE_LOST, &SecondarySubtitleStrip::OnMouseCaptureLost, this);
	Bind(wxEVT_MOUSEWHEEL, &SecondarySubtitleStrip::OnMouseWheel, this);

	scroll_bar->Hide();
	scroll_bar->Bind(wxEVT_MOUSEWHEEL, &SecondarySubtitleStrip::OnMouseWheel, this);
	entry_button->Bind(wxEVT_MOUSEWHEEL, &SecondarySubtitleStrip::OnMouseWheel, this);
	reload_button->Bind(wxEVT_MOUSEWHEEL, &SecondarySubtitleStrip::OnMouseWheel, this);
}

wxRect SecondarySubtitleStrip::GetGutterRect() const {
	wxRect rect = GetClientRect();
	rect.width = std::clamp(left_gutter_width, 0, rect.width);
	return rect;
}

wxRect SecondarySubtitleStrip::GetContentRect() const {
	wxRect rect = GetClientRect();
	int gutter_width = std::clamp(left_gutter_width, 0, rect.width);
	rect.x += gutter_width;
	rect.width = std::max(0, rect.width - gutter_width);
	return rect;
}

wxRect SecondarySubtitleStrip::GetResizeHandleRect() const {
	wxRect rect = GetContentRect();
	rect.height = std::min(rect.height, FromDIP(10));
	return rect;
}

wxRect SecondarySubtitleStrip::GetRulerOverlayRect(wxRect const& content_rect) const {
	if (!OPT_GET("Video/Secondary Subtitles/Show Vertical Ruler")->GetBool())
		return wxRect();

	wxRect rect = content_rect;
	rect.width = std::min(GetRulerOverlayWidth(), rect.width);
	rect.x = rect.GetRight() - rect.width + 1;
	return rect;
}
int SecondarySubtitleStrip::GetVisibleSourceHeightForBitmap(wxBitmap const& bitmap, wxRect const& content_rect) const {
	if (!bitmap.IsOk() || bitmap.GetWidth() <= 0 || bitmap.GetHeight() <= 0)
		return 0;
	if (content_rect.width <= 0 || content_rect.height <= 0)
		return 0;

	// Keep the same scale factor as the displayed video width so the strip is
	// a cropped view rather than a vertically stretched resample.
	double scale = static_cast<double>(content_rect.width) / bitmap.GetWidth();
	if (scale <= 0.0)
		return 0;

	return std::clamp(
		static_cast<int>(std::ceil(content_rect.height / scale)),
		kMinimumVisibleSourceHeight,
		bitmap.GetHeight());
}

SecondarySubtitleStripLayout SecondarySubtitleStrip::BuildLayoutForBitmap(wxBitmap const& bitmap, wxRect const& content_rect) const {
	return BuildSecondarySubtitleStripLayout(
		bitmap.IsOk() ? bitmap.GetHeight() : 0,
		GetVisibleSourceHeightForBitmap(bitmap, content_rect),
		scroll_offset_y);
}
void SecondarySubtitleStrip::ApplyConfiguredHeight(int logical_height) {
	int panel_height = std::clamp(logical_height, kMinimumPanelHeight, kMaximumPanelHeight);
	int dip_height = FromDIP(panel_height);
	SetMinSize(wxSize(-1, dip_height));
	SetInitialSize(wxSize(-1, dip_height));
	InvalidateBestSize();
}

int SecondarySubtitleStrip::GetRulerOverlayWidth() const {
	return OPT_GET("Video/Secondary Subtitles/Show Vertical Ruler")->GetBool() ? FromDIP(44) : 0;
}

void SecondarySubtitleStrip::StoreScrollOffset() {
	int option_value = static_cast<int>(OPT_GET("Video/Secondary Subtitles/Scroll Offset Y")->GetInt());
	if (option_value != scroll_offset_y)
		OPT_SET("Video/Secondary Subtitles/Scroll Offset Y")->SetInt(scroll_offset_y);
}

int SecondarySubtitleStrip::GetEntryButtonIconSize() const {
	return GetVideoUiIconSize(
		const_cast<SecondarySubtitleStrip *>(this),
		OPT_GET("App/Toolbar Icon Size")->GetInt());
}

void SecondarySubtitleStrip::RefreshGutterToolbars() {
	int icon_size = std::max(GetEntryButtonIconSize(), 1);
	entry_button->ClearTools();
	entry_button->SetToolBitmapSize(wxSize(icon_size, icon_size));
	entry_button->AddTool(
		MenuSecondarySubtitleStripOptionsButton,
		_("Secondary subtitle strip options"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(options_button, GetLayoutDirection(), icon_size)),
		_("Secondary subtitle strip options"));
	entry_button->Realize();
	entry_button->SetToolTip(_("Secondary subtitle strip options"));
	int const entry_button_extent = GetGutterButtonExtent(this, entry_button);
	entry_button->SetMinSize(wxSize(entry_button_extent, entry_button_extent));

	reload_button->ClearTools();
	reload_button->SetToolBitmapSize(wxSize(icon_size, icon_size));
	reload_button->AddTool(
		MenuSecondarySubtitleStripReloadButton,
		_("Reload secondary source"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(arrow_sort, GetLayoutDirection(), icon_size)),
		_("Reload secondary source"));
	reload_button->Realize();
	reload_button->SetToolTip(_("Reload secondary source"));
	int const reload_button_extent = GetGutterButtonExtent(this, reload_button);
	reload_button->SetMinSize(wxSize(reload_button_extent, reload_button_extent));
}

void SecondarySubtitleStrip::LayoutGutterControls() {
	wxRect gutter_rect = GetGutterRect();
	int margin = FromDIP(2);
	int button_spacing = FromDIP(1);
	int available_width = std::max(gutter_rect.width - margin * 2, 0);
	auto position_button = [&](wxToolBar *button, int top_y) -> int {
		auto best_size = button->GetEffectiveMinSize();
		int button_width = std::min(
			available_width,
			std::max(best_size.GetWidth(), FromDIP(20)));
		int button_height = std::max(best_size.GetHeight(), FromDIP(20));
		int max_button_height = std::max(gutter_rect.height - top_y - margin, 0);
		button_height = std::min(button_height, max_button_height);
		button->Show(button_width > 0 && button_height > 0);
		if (button_width > 0 && button_height > 0) {
			int button_x = gutter_rect.x + std::max((gutter_rect.width - button_width) / 2, 0);
			button->SetSize(button_x, top_y, button_width, button_height);
			return top_y + button_height;
		}
		return top_y;
	};

	int next_y = margin;
	next_y = position_button(entry_button, next_y);
	if (entry_button->IsShown())
		next_y += button_spacing;
	next_y = position_button(reload_button, next_y);
	if (reload_button->IsShown())
		next_y += margin;

	int scroll_width = std::min(GetScrollBarWidth(this), available_width);
	int scroll_x = gutter_rect.x + std::max((gutter_rect.width - scroll_width) / 2, 0);
	int scroll_y = next_y;
	int scroll_bottom = gutter_rect.GetBottom() - margin + 1;
	int scroll_height = std::max(scroll_bottom - scroll_y, 0);
	scroll_bar->SetSize(scroll_x, scroll_y, scroll_width, scroll_height);
}

int SecondarySubtitleStrip::GetThumbPosition(int max_scroll_offset_y) const {
	return SecondarySubtitleStripThumbPositionFromScrollOffset(scroll_offset_y, max_scroll_offset_y);
}

int SecondarySubtitleStrip::GetScrollOffsetForThumbPosition(int thumb_position, int max_scroll_offset_y) const {
	return SecondarySubtitleStripScrollOffsetFromThumbPosition(thumb_position, max_scroll_offset_y);
}

void SecondarySubtitleStrip::UpdateScrollBar() {
	if (!session->HasBitmap()) {
		scroll_bar->Hide();
		return;
	}

	auto const& bitmap = session->GetBitmap();
	auto layout = BuildLayoutForBitmap(bitmap, GetContentRect());
	// Keep the requested bottom distance across resizes even when the current
	// strip height can only display a clamped view.

	bool const show_scroll = layout.max_scroll_offset_y > 0;
	scroll_bar->Show(show_scroll);
	if (show_scroll) {
		scroll_bar->SetScrollbar(
			GetThumbPosition(layout.max_scroll_offset_y),
			layout.source_height,
			bitmap.GetHeight(),
			layout.source_height,
			true);
	}
}

void SecondarySubtitleStrip::ScrollBySourceDelta(int delta_y) {
	if (!session->HasBitmap())
		return;

	auto const& bitmap = session->GetBitmap();
	auto layout = BuildLayoutForBitmap(bitmap, GetContentRect());
	if (layout.max_scroll_offset_y <= 0)
		return;

	scroll_offset_y = std::clamp(
		layout.clamped_scroll_offset_y + delta_y,
		0,
		layout.max_scroll_offset_y);
	StoreScrollOffset();
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::OnEntryButton(wxCommandEvent &) {
	wxMenu menu;
	auto const subtitles_providers = SubtitlesProviderFactory::GetClasses();
	auto const effective_provider = session->GetEffectiveSubtitlesProvider();
	bool const follows_preferences = session->IsFollowingGlobalSubtitlesProvider();
	menu.Append(MenuUseCurrentScript, _("Use Current Script"));
	menu.Append(MenuOpenExternalSubtitles, _("Open Secondary Subtitles..."));
	menu.AppendSeparator();
	auto *provider_menu = new wxMenu;
	provider_menu->AppendCheckItem(
		MenuFollowPreferencesSubtitlesProvider,
		to_wx("Follow Preferences Provider (" + effective_provider + ")"));
	provider_menu->Check(MenuFollowPreferencesSubtitlesProvider, follows_preferences);
	provider_menu->AppendSeparator();
	auto configured_provider = session->GetConfiguredSubtitlesProvider();
	for (size_t i = 0; i < subtitles_providers.size(); ++i) {
		int provider_id = MenuSecondarySubtitleProviderFirst + static_cast<int>(i);
		auto item = provider_menu->AppendRadioItem(provider_id, to_wx(subtitles_providers[i]));
		bool checked = follows_preferences
			? effective_provider == subtitles_providers[i]
			: configured_provider == subtitles_providers[i];
		item->Check(checked);
	}
	menu.AppendSubMenu(provider_menu, _("Subtitles Provider"));
	menu.AppendSeparator();
	menu.Append(MenuConfigureSecondarySubtitleStrip, _("Settings..."));

	auto selection = GetPopupMenuSelectionFromUser(
		menu,
		entry_button->GetPosition() + wxPoint(0, entry_button->GetSize().GetHeight()));
	switch (selection) {
	case MenuUseCurrentScript:
		session->UseCurrentScriptSource();
		break;
	case MenuOpenExternalSubtitles:
		session->OpenExternalSubtitles();
		break;
	case MenuFollowPreferencesSubtitlesProvider:
		session->UseGlobalSubtitlesProvider();
		break;
	case MenuConfigureSecondarySubtitleStrip:
		ShowSecondarySubtitleStripSettings(this, kMinimumPanelHeight, kMaximumPanelHeight);
		break;
	default:
		if (selection >= MenuSecondarySubtitleProviderFirst
			&& selection < MenuSecondarySubtitleProviderFirst + static_cast<int>(subtitles_providers.size())) {
			session->UseIndependentSubtitlesProvider(
				subtitles_providers[selection - MenuSecondarySubtitleProviderFirst]);
			break;
		}
		return;
	}

	RefreshGutterToolbars();
	LayoutGutterControls();
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::OnReloadButton(wxCommandEvent &) {
	session->ReloadSubtitles();
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::OnConfiguredHeightChanged(agi::OptionValue const& opt) {
	int previous_height = GetSize().GetHeight();
	ApplyConfiguredHeight(static_cast<int>(opt.GetInt()));
	if (auto *parent = GetParent())
		parent->InvalidateBestSize();
	if (auto *video_box = dynamic_cast<VideoBox *>(GetParent())) {
		video_box->OnSecondarySubtitleStripHeightChanged(previous_height, GetMinSize().GetHeight());
	}
	else if (auto *parent = GetParent()) {
		parent->Layout();
		parent->Refresh();
		parent->Update();
		parent->SendSizeEvent(wxSEND_EVENT_POST);
	}
	LayoutGutterControls();
	Layout();
	Refresh(false);
	if (!resize_dragging)
		Update();
	if (!dynamic_cast<VideoBox *>(GetParent())) {
		if (auto *top = wxGetTopLevelParent(this))
			top->SendSizeEvent(wxSEND_EVENT_POST);
	}
}

void SecondarySubtitleStrip::OnConfiguredScrollOffsetChanged(agi::OptionValue const& opt) {
	scroll_offset_y = std::max(static_cast<int>(opt.GetInt()), 0);
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::OnToolbarIconSizeChanged(agi::OptionValue const&) {
	RefreshGutterToolbars();
	LayoutGutterControls();
	Refresh(false);
}

void SecondarySubtitleStrip::OnVideoDpiScaleChanged(agi::OptionValue const&) {
	RefreshGutterToolbars();
	LayoutGutterControls();
	Refresh(false);
}

void SecondarySubtitleStrip::OnConfiguredShowVerticalRulerChanged(agi::OptionValue const&) {
	Refresh(false);
}

void SecondarySubtitleStrip::OnPaint(wxPaintEvent &) {
	wxAutoBufferedPaintDC dc(this);
	dc.SetBackground(wxBrush(GetBackgroundColour()));
	dc.Clear();

	auto panel_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_3DFACE);
	auto shadow_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);
	wxRect gutter_rect = GetGutterRect();
	if (gutter_rect.width > 0 && gutter_rect.height > 0) {
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(panel_colour));
		dc.DrawRectangle(gutter_rect);
		dc.SetPen(wxPen(shadow_colour));
		dc.DrawLine(gutter_rect.GetRight(), gutter_rect.GetTop(), gutter_rect.GetRight(), gutter_rect.GetBottom());
	}

	wxRect content_rect = GetContentRect();
	if (content_rect.width > 0 && content_rect.height > 0 && session->HasBitmap()) {
		auto const& bitmap = session->GetBitmap();
		auto layout = BuildLayoutForBitmap(bitmap, content_rect);
		if (layout.source_height > 0) {
			double scale = static_cast<double>(content_rect.width) / bitmap.GetWidth();
			int draw_height = std::clamp(
				static_cast<int>(std::lround(layout.source_height * scale)),
				1,
				content_rect.height);
			int draw_y = content_rect.y + content_rect.height - draw_height;

			wxBitmap source_bitmap = bitmap.GetSubBitmap(wxRect(0, layout.source_top, bitmap.GetWidth(), layout.source_height));
			bool drew_bitmap = false;
			if (source_bitmap.IsOk()) {
				if (source_bitmap.GetWidth() == content_rect.width && source_bitmap.GetHeight() == draw_height) {
					dc.DrawBitmap(source_bitmap, content_rect.x, draw_y, false);
					drew_bitmap = true;
				}
				else {
					wxImage source_image = source_bitmap.ConvertToImage();
					if (source_image.IsOk()) {
						wxImage scaled_image = source_image.Scale(content_rect.width, draw_height, wxIMAGE_QUALITY_HIGH);
						if (scaled_image.IsOk()) {
							dc.DrawBitmap(wxBitmap(scaled_image), content_rect.x, draw_y, false);
							drew_bitmap = true;
						}
					}
				}
			}
			if (!drew_bitmap) {
				wxBitmap paint_bitmap = bitmap;
				wxMemoryDC memory_dc;
				memory_dc.SelectObject(paint_bitmap);
				dc.StretchBlit(
					content_rect.x,
					draw_y,
					content_rect.width,
					draw_height,
					&memory_dc,
					0,
					layout.source_top,
					bitmap.GetWidth(),
					layout.source_height);
			}

			wxRect ruler_rect = GetRulerOverlayRect(content_rect);
			if (ruler_rect.width > 0 && ruler_rect.height > 0) {
				auto ruler_text_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
				auto ruler_shadow_colour = wxColour(0, 0, 0);
				dc.SetPen(wxPen(shadow_colour));
				dc.SetTextForeground(ruler_text_colour);

				wxFont ruler_font = GetFont();
				int point_size = ruler_font.GetPointSize();
				if (point_size > 7)
					ruler_font.SetPointSize(point_size - 1);
				dc.SetFont(ruler_font);

				int desired_labels = std::max(draw_height / FromDIP(48), 2);
				int major_step = PickNiceRulerStep(static_cast<double>(layout.source_height) / desired_labels);
				int source_bottom = layout.source_top + layout.source_height - 1;
				int first_tick = ((layout.source_top + major_step - 1) / major_step) * major_step;
				int tick_length = FromDIP(7);
				int label_padding = FromDIP(3);
				int last_label_bottom = std::numeric_limits<int>::min();

				for (int value = first_tick; value <= source_bottom; value += major_step) {
					double t = static_cast<double>(value - layout.source_top) / layout.source_height;
					int y = draw_y + static_cast<int>(std::lround(t * draw_height));
					y = std::clamp(y, draw_y, draw_y + draw_height - 1);
					dc.DrawLine(ruler_rect.x + FromDIP(2), y, ruler_rect.x + tick_length + FromDIP(2), y);

					wxString label = wxString::Format(wxS("%d"), value);
					int text_width = 0;
					int text_height = 0;
					dc.GetTextExtent(label, &text_width, &text_height);
					int min_text_y = std::max(draw_y + FromDIP(1), ruler_rect.y + FromDIP(1));
					int max_text_y = std::max(
						min_text_y,
						std::min(draw_y + draw_height - text_height, ruler_rect.GetBottom() - text_height));
					int text_y = std::clamp(y - text_height / 2, min_text_y, max_text_y);
					if (text_y <= last_label_bottom + FromDIP(2))
						continue;

					int text_x = ruler_rect.x + tick_length + label_padding;
					if (text_x + text_width > ruler_rect.GetRight() + 1)
						text_x = std::max(ruler_rect.GetRight() - text_width + 1, ruler_rect.x + tick_length);
					dc.SetTextForeground(ruler_shadow_colour);
					dc.DrawText(label, text_x + 1, text_y + 1);
					dc.SetTextForeground(ruler_text_colour);
					dc.DrawText(label, text_x, text_y);
					last_label_bottom = text_y + text_height;
				}
			}
		}
	}

	wxRect resize_handle_rect = GetResizeHandleRect();
	if (resize_handle_rect.width > 0 && resize_handle_rect.height > 0) {
		auto handle_line_colour = shadow_colour.ChangeLightness(resize_dragging ? 85 : (resize_handle_hot ? 100 : 115));
		auto grip_colour = resize_dragging
			? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
			: shadow_colour.ChangeLightness(resize_handle_hot ? 85 : 100);
		dc.SetPen(wxPen(handle_line_colour));
		dc.DrawLine(
			resize_handle_rect.x,
			resize_handle_rect.GetTop(),
			resize_handle_rect.GetRight(),
			resize_handle_rect.GetTop());

		int grip_width = std::min(std::max(FromDIP(28), resize_handle_rect.width / 5), FromDIP(56));
		int grip_height = std::max(FromDIP(3), 2);
		wxRect grip_rect(
			resize_handle_rect.x + std::max((resize_handle_rect.width - grip_width) / 2, 0),
			resize_handle_rect.y + std::max((resize_handle_rect.height - grip_height) / 2, 0),
			grip_width,
			grip_height);
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(grip_colour));
			dc.DrawRoundedRectangle(grip_rect.x, grip_rect.y, grip_rect.width, grip_rect.height, grip_rect.height / 2.0);
		}
	}

void SecondarySubtitleStrip::OnSize(wxSizeEvent &event) {
	LayoutGutterControls();
	UpdateScrollBar();
	Refresh(false);
	event.Skip();
}

void SecondarySubtitleStrip::OnScroll(wxScrollEvent &) {
	if (!session->HasBitmap())
		return;

	auto const& bitmap = session->GetBitmap();
	auto layout = BuildLayoutForBitmap(bitmap, GetContentRect());
	scroll_offset_y = GetScrollOffsetForThumbPosition(
		scroll_bar->GetThumbPosition(),
		layout.max_scroll_offset_y);
	StoreScrollOffset();
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::UpdateResizeHandleHot(wxPoint const& position, bool mouse_present) {
	bool hot = mouse_present && GetResizeHandleRect().Contains(position);
	if (resize_handle_hot == hot)
		return;

	resize_handle_hot = hot;
	if (!resize_dragging)
		SetCursor(resize_handle_hot ? wxCursor(wxCURSOR_SIZENS) : wxNullCursor);
	Refresh(false);
}

void SecondarySubtitleStrip::FinishMouseInteractions() {
	resize_dragging = false;
	middle_dragging = false;
	if (HasCapture())
		ReleaseMouse();
	SetCursor(resize_handle_hot ? wxCursor(wxCURSOR_SIZENS) : wxNullCursor);
}

void SecondarySubtitleStrip::SetConfiguredHeightFromDragScreenY(int screen_y) {
	int delta_y = screen_y - resize_drag_start_screen_y;
	double scale_factor = std::max(GetWindowScaleFactor(this), 0.001);
	int logical_delta_y = static_cast<int>(std::lround(delta_y / scale_factor));
	int target_height = std::clamp(
		resize_drag_initial_height + logical_delta_y,
		kMinimumPanelHeight,
		kMaximumPanelHeight);
	if (target_height != static_cast<int>(OPT_GET("Video/Secondary Subtitles/Height")->GetInt()))
		OPT_SET("Video/Secondary Subtitles/Height")->SetInt(target_height);
}

void SecondarySubtitleStrip::OnMouseWheel(wxMouseEvent &event) {
	if (!session->HasBitmap() || event.GetWheelAxis() != 0) {
		event.Skip();
		return;
	}

	int wheel_delta = event.GetWheelDelta();
	if (wheel_delta == 0)
		return;

	wheel_scroll_accum += event.GetWheelRotation();
	int steps = wheel_scroll_accum / wheel_delta;
	wheel_scroll_accum %= wheel_delta;
	if (steps == 0)
		return;

	auto const& bitmap = session->GetBitmap();
	auto layout = BuildLayoutForBitmap(bitmap, GetContentRect());
	int lines = std::max(event.GetLinesPerAction(), 1);
	int source_pixels_per_line = std::max(layout.source_height / 30, 1);
	ScrollBySourceDelta(steps * lines * source_pixels_per_line);
}

void SecondarySubtitleStrip::OnMouseEvent(wxMouseEvent &event) {
	wxPoint position = event.GetPosition();

	if (event.LeftDown()) {
		if (!GetResizeHandleRect().Contains(position)) {
			event.Skip();
			return;
		}

		if (auto *video_box = dynamic_cast<VideoBox *>(GetParent()))
			video_box->BeginSecondarySubtitleStripHeightDrag();
		resize_dragging = true;
		resize_handle_hot = true;
		resize_drag_start_screen_y = ClientToScreen(position).y;
		resize_drag_initial_height = std::clamp(
			static_cast<int>(OPT_GET("Video/Secondary Subtitles/Height")->GetInt()),
			kMinimumPanelHeight,
			kMaximumPanelHeight);
		if (!HasCapture())
			CaptureMouse();
		SetCursor(wxCursor(wxCURSOR_SIZENS));
		Refresh(false);
		return;
	}

	if (event.LeftUp()) {
		if (!resize_dragging) {
			UpdateResizeHandleHot(position, true);
			event.Skip();
			return;
		}

		SetConfiguredHeightFromDragScreenY(ClientToScreen(position).y);
		resize_dragging = false;
		if (!middle_dragging && HasCapture())
			ReleaseMouse();
		if (auto *video_box = dynamic_cast<VideoBox *>(GetParent()))
			video_box->CommitSecondarySubtitleStripHeightDrag();
		UpdateResizeHandleHot(position, true);
		return;
	}

	if (resize_dragging) {
		if (!event.LeftIsDown()) {
			FinishMouseInteractions();
			return;
		}

		if (event.Dragging() || event.Moving()) {
			SetConfiguredHeightFromDragScreenY(ClientToScreen(position).y);
			return;
		}
	}

	if (event.MiddleDown()) {
		middle_dragging = true;
		last_drag_y = event.GetY();
		if (!HasCapture())
			CaptureMouse();
		return;
	}

	if (event.MiddleUp()) {
		middle_dragging = false;
		if (!resize_dragging && HasCapture())
			ReleaseMouse();
		return;
	}

	if (event.Leaving()) {
		UpdateResizeHandleHot(position, false);
		if (!event.MiddleIsDown())
			middle_dragging = false;
		return;
	}

	if (event.Moving())
		UpdateResizeHandleHot(position, true);

	if (!session->HasBitmap())
		return;

	if (!event.MiddleIsDown())
		middle_dragging = false;

	if (!middle_dragging || !event.Dragging() || !event.MiddleIsDown())
		return;

	wxRect content_rect = GetContentRect();
	if (content_rect.height <= 0)
		return;

	auto const& bitmap = session->GetBitmap();
	auto layout = BuildLayoutForBitmap(bitmap, content_rect);
	if (layout.max_scroll_offset_y <= 0)
		return;

	int delta_y = event.GetY() - last_drag_y;
	last_drag_y = event.GetY();
	if (delta_y == 0)
		return;

	double source_per_pixel = static_cast<double>(layout.source_height) / content_rect.height;
	int scroll_delta = static_cast<int>(std::lround(std::abs(delta_y) * source_per_pixel));
	scroll_delta = std::max(scroll_delta, 1);
	if (delta_y > 0)
		scroll_delta = -scroll_delta;

	scroll_offset_y = std::clamp(
		scroll_offset_y + scroll_delta,
		0,
		layout.max_scroll_offset_y);
	StoreScrollOffset();
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
	if (resize_dragging) {
		if (auto *video_box = dynamic_cast<VideoBox *>(GetParent()))
			video_box->CommitSecondarySubtitleStripHeightDrag();
	}
	FinishMouseInteractions();
}

bool SecondarySubtitleStrip::OpenExternalSubtitlesFromPath(agi::fs::path const& path, bool show_errors) {
	return session && session->OpenExternalSubtitlesFromPath(path, show_errors);
}

void SecondarySubtitleStrip::SetSessionActive(bool active) {
	if (!active) {
		if (resize_dragging) {
			if (auto *video_box = dynamic_cast<VideoBox *>(GetParent()))
				video_box->CommitSecondarySubtitleStripHeightDrag();
		}
		FinishMouseInteractions();
	}

	session->SetActive(active);
	UpdateScrollBar();
	Refresh(false);
}

void SecondarySubtitleStrip::SetLeftGutterWidth(int width) {
	int clamped_width = std::max(width, 0);
	if (left_gutter_width == clamped_width)
		return;

	left_gutter_width = clamped_width;
	LayoutGutterControls();
	Refresh(false);
}
