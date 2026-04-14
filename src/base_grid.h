// Copyright (c) 2006, Rodrigo Braz Monteiro
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

#include <libaegisub/signal.h>

#include <memory>
#include <string>
#include <vector>
#include <wx/window.h>

#include "time_display_mode.h"

namespace agi {
	struct Context;
	class OptionValue;
}
class AssDialogue;
class GridColumn;
class WidthHelper;

class BaseGrid final : public wxWindow {
	std::vector<agi::signal::Connection> connections;
	int lineHeight = 1;     ///< Height of a line in pixels in the current font
	bool holding = false;   ///< Is a drag selection in process?
	wxFont font;            ///< Current grid font
	wxScrollBar *scrollBar; ///< The grid's scrollbar
	SubtitleTimeDisplayMode display_mode = SubtitleTimeDisplayMode::Ass;

	/// Row from which the selection shrinks/grows from when selecting via the
	/// keyboard, shift-clicking or dragging
	int extendRow = -1;

	/// First row that is visible at the current scroll position
	int yPos = 0;

	int active_row = -1;
	int current_frame = -1;

	std::unique_ptr<WidthHelper> width_helper;

	/// Rows which are visible on the current video frame
	std::vector<int> visible_rows;

	agi::Context *context; ///< Associated project context

	std::vector<std::unique_ptr<GridColumn>> columns;
	std::vector<bool> columns_visible;

	std::vector<wxRect> text_refresh_rects;

	bool refresh_on_idle = false;

	/// Cached brushes used for row backgrounds
	struct {
		wxBrush Default;
		wxBrush Header;
		wxBrush Selection;
		wxBrush Comment;
		wxBrush Visible;
		wxBrush SelectedComment;
		wxBrush LeftCol;
	} row_colors;

	std::vector<AssDialogue*> index_line_map;  ///< Row number -> dialogue line

	/// Cached grid body context menu
	std::unique_ptr<wxMenu> context_menu;

	void OnContextMenu(wxContextMenuEvent &evt);
	void OnHighlightVisibleChange(agi::OptionValue const& opt);
	void OnIdle(wxIdleEvent&);
	void OnKeyDown(wxKeyEvent &event);
	void OnCharHook(wxKeyEvent &event);
	void OnMouseEvent(wxMouseEvent &event);
	void OnPaint(wxPaintEvent &event);
	void OnScroll(wxScrollEvent &event);
	void OnShowColMenu(wxCommandEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnSubtitlesCommit(int type, const AssDialogue *single_line);
	void OnActiveLineChanged(AssDialogue *);
	void OnCurrentFrameChanged(int frame_number);
	void OnVideoProviderChanged();

	void AdjustScrollbar();
	std::vector<int> GetRowsDisplayedAtCurrentFrame() const;
	wxRect GetScrollableRect() const;
	void RefreshChangedVisibleRows(std::vector<int> const& old_visible_rows, std::vector<int> const& new_visible_rows);
	void RefreshAfterScroll(int old_y_pos);
	void RefreshDialogueRow(const AssDialogue *line);
	void SetColumnWidths();

	bool IsDisplayed(const AssDialogue *line) const;

	void UpdateMaps();
	void UpdateStyle();

	void SelectRow(int row, bool addToSelected = false, bool select=true);

	int GetRows() const { return index_line_map.size(); }
	void MakeRowVisible(int row);

	/// @brief Get dialogue by index
	/// @param n Index to look up
	/// @return Subtitle dialogue line for index, or 0 if invalid index
	AssDialogue *GetDialogue(int n) const;

public:
	BaseGrid(wxWindow* parent, agi::Context *context);
	~BaseGrid();

	void SetDisplayMode(SubtitleTimeDisplayMode mode);
	void SetByFrame(bool state);
	void ScrollTo(int y);

	DECLARE_EVENT_TABLE()
};
