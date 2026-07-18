// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "secondary_subtitle_strip_layout.h"

#include <libaegisub/fs_fwd.h>
#include <libaegisub/signal.h>

#include <cstdint>
#include <memory>
#include <wx/bitmap.h>
#include <wx/panel.h>

class SecondarySubtitleSession;
class wxScrollBar;
class wxToolBar;
namespace agi {
	struct Context;
	class OptionValue;
}

class SecondarySubtitleStrip final : public wxPanel {
	std::unique_ptr<SecondarySubtitleSession> session;
	wxToolBar *entry_button = nullptr;
	wxToolBar *reload_button = nullptr;
	wxScrollBar *scroll_bar = nullptr;
	agi::signal::Connection height_option_connection;
	agi::signal::Connection scroll_offset_option_connection;
	agi::signal::Connection toolbar_icon_size_option_connection;
	agi::signal::Connection video_dpi_scale_option_connection;
	agi::signal::Connection show_vertical_ruler_option_connection;
	int left_gutter_width = 0;
	int scroll_offset_y = 0;
	int wheel_scroll_accum = 0;
	bool resize_dragging = false;
	bool resize_handle_hot = false;
	int resize_drag_start_screen_y = 0;
	int resize_drag_initial_height = 0;
	bool middle_dragging = false;
	int last_drag_y = 0;
	wxBitmap paint_bitmap_cache;
	std::uint64_t paint_bitmap_cache_generation = 0;
	int paint_bitmap_cache_source_top = 0;
	int paint_bitmap_cache_source_height = 0;
	int paint_bitmap_cache_width = 0;
	int paint_bitmap_cache_height = 0;

	static constexpr int kMinimumPanelHeight = 48;
	static constexpr int kMaximumPanelHeight = 480;
	static constexpr int kMinimumVisibleSourceHeight = 1;

	wxRect GetGutterRect() const;
	wxRect GetContentRect() const;
	wxRect GetResizeHandleRect() const;
	wxRect GetRulerOverlayRect(wxRect const& content_rect) const;
	int GetVisibleSourceHeightForBitmap(wxBitmap const& bitmap, wxRect const& content_rect) const;
	SecondarySubtitleStripLayout BuildLayoutForBitmap(wxBitmap const& bitmap, wxRect const& content_rect) const;
	void ApplyConfiguredHeight(int logical_height);
	int GetRulerOverlayWidth() const;
	void LayoutGutterControls();
	void StoreScrollOffset();
	int GetEntryButtonIconSize() const;
	void RefreshGutterToolbars();
	void UpdateScrollBar();
	void UpdateResizeHandleHot(wxPoint const& position, bool mouse_present);
	void FinishMouseInteractions();
	void SetConfiguredHeightFromDragScreenY(int screen_y);
	void ScrollBySourceDelta(int delta_y);
	int GetThumbPosition(int max_scroll_offset_y) const;
	int GetScrollOffsetForThumbPosition(int thumb_position, int max_scroll_offset_y) const;
	void OnEntryButton(wxCommandEvent &event);
	void OnReloadButton(wxCommandEvent &event);
	void OnConfiguredHeightChanged(agi::OptionValue const& opt);
	void OnConfiguredScrollOffsetChanged(agi::OptionValue const& opt);
	void OnToolbarIconSizeChanged(agi::OptionValue const& opt);
	void OnVideoDpiScaleChanged(agi::OptionValue const& opt);
	void OnConfiguredShowVerticalRulerChanged(agi::OptionValue const& opt);
	void OnPaint(wxPaintEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnScroll(wxScrollEvent &event);
	void OnMouseWheel(wxMouseEvent &event);
	void OnMouseEvent(wxMouseEvent &event);
	void OnMouseCaptureLost(wxMouseCaptureLostEvent &event);

public:
	SecondarySubtitleStrip(wxWindow *parent, agi::Context *context);

	bool OpenExternalSubtitlesFromPath(agi::fs::path const& path, bool show_errors = true);
	void SetSessionActive(bool active);
	void SetLeftGutterWidth(int width);
};
