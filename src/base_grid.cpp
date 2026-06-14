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

#include "base_grid.h"

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_box.h"
#include "compat.h"
#include "grid_core/grid_layout.h"
#include "grid_column.h"
#include "grid_column_painter.h"
#include "options.h"
#include "project.h"
#include "utils.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "video_controller.h"

#include <libaegisub/make_unique.h>
#include <libaegisub/util.h>

#include <algorithm>

#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>

// Check menu.h for id range allocation before editing this enum
enum {
	GRID_SCROLLBAR = 1730,
	MENU_SHOW_COL = (wxID_HIGHEST + 1) + 2000 // Needs 15 IDs after this
};

BaseGrid::BaseGrid(wxWindow* parent, agi::Context *context)
: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxSUNKEN_BORDER)
, scrollBar(new wxScrollBar(this, GRID_SCROLLBAR, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL))
, context(context)
, current_frame(context->GetCore().videoController->GetFrameN())
, columns(GetGridColumns())
, columns_visible(OPT_GET("Subtitle/Grid/Column")->GetListBool())
{
	scrollBar->SetScrollbar(0,10,100,10);

	auto scrollbarpositioner = new wxBoxSizer(wxHORIZONTAL);
	scrollbarpositioner->AddStretchSpacer();
	scrollbarpositioner->Add(scrollBar, 0, wxEXPAND, 0);

	SetSizerAndFit(scrollbarpositioner);

	SetBackgroundStyle(wxBG_STYLE_PAINT);

	for (size_t i : agi::util::range(std::min(columns_visible.size(), columns.size()))) {
		if (!columns_visible[i])
			columns[i]->SetVisible(false);
	}

	UpdateStyle();
	OnHighlightVisibleChange(*OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame"));

	auto core = context->GetCore();
	connections = agi::signal::make_vector({
		core.ass->AddCommitListener(&BaseGrid::OnSubtitlesCommit, this),

		core.selectionController->AddActiveLineListener(&BaseGrid::OnActiveLineChanged, this),
		core.selectionController->AddSelectionListener([&]{ Refresh(false); }),
		core.project->AddVideoProviderListener(&BaseGrid::OnVideoProviderChanged, this),
		core.videoController->AddFramePresentedListener(&BaseGrid::OnCurrentFrameChanged, this),

		OPT_SUB("Subtitle/Grid/Font Face", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Subtitle/Grid/Font Size", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Active Border", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Background", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Comment", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Inframe", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Selected Comment", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Selection", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Collision", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Header", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Left Column", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Lines", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Selection", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Standard", &BaseGrid::UpdateStyle, this),

		OPT_SUB("Subtitle/Grid/Highlight Subtitles in Frame", &BaseGrid::OnHighlightVisibleChange, this),
		OPT_SUB("Subtitle/Grid/Hide Overrides", [&](agi::OptionValue const&) { Refresh(false); }),
		OPT_SUB("Subtitle/Character Counter/Show Decimal CPS", &BaseGrid::UpdateStyle, this),
	});

	Bind(wxEVT_CONTEXT_MENU, &BaseGrid::OnContextMenu, this);
}

BaseGrid::~BaseGrid() { }

BEGIN_EVENT_TABLE(BaseGrid,wxWindow)
	EVT_PAINT(BaseGrid::OnPaint)
	EVT_SIZE(BaseGrid::OnSize)
	EVT_COMMAND_SCROLL(GRID_SCROLLBAR,BaseGrid::OnScroll)
	EVT_MOUSE_EVENTS(BaseGrid::OnMouseEvent)
	EVT_KEY_DOWN(BaseGrid::OnKeyDown)
	EVT_CHAR_HOOK(BaseGrid::OnCharHook)
	EVT_MENU_RANGE(MENU_SHOW_COL,MENU_SHOW_COL+15,BaseGrid::OnShowColMenu)
	EVT_IDLE(BaseGrid::OnIdle)
END_EVENT_TABLE()

void BaseGrid::OnSubtitlesCommit(int type, const AssDialogue *single_line) {
	if (type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_ORDER || type & AssFile::COMMIT_DIAG_ADDREM)
		UpdateMaps();

	if (type & AssFile::COMMIT_DIAG_META) {
		SetColumnWidths();
		Refresh(false);
		return;
	}
	if (type & AssFile::COMMIT_DIAG_TIME) {
		// Dragging start / end time in audio display can generate lots of commit in a short period of time.
		// On the other hand, GDI painting time depends on area, and BaseGrid typically is very large. Therefore repainting BaseGrid can be expensive.
		// To prevent GUI lag / FPS drop caused by frequent repaint of BaseGrid, we do not call Refresh(false) here. Instead, we set the refresh_on_idle flag, and only repaint BaseGrid when idle.
		refresh_on_idle = true;
	}
	else if (type & AssFile::COMMIT_DIAG_TEXT) {
		if (single_line) {
			RefreshDialogueRow(single_line);
			return;
		}

		for (auto const& rect : text_refresh_rects)
			RefreshRect(rect, false);
	}
}

void BaseGrid::OnShowColMenu(wxCommandEvent &event) {
	int item = event.GetId() - MENU_SHOW_COL;
	bool new_value = !columns_visible[item];

	columns_visible.resize(columns.size(), true);
	columns_visible[item] = new_value;
	OPT_SET("Subtitle/Grid/Column")->SetListBool(columns_visible);
	columns[item]->SetVisible(new_value);

	SetColumnWidths();

	Refresh(false);
}

void BaseGrid::OnHighlightVisibleChange(agi::OptionValue const& opt) {
	(void)opt;
	Refresh(false);
}

void BaseGrid::UpdateStyle() {
	wxString fontname = FontFace("Subtitle/Grid");
	if (fontname.empty()) fontname = wxS("Tahoma");
	font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Grid/Font Size")->GetInt());
	font.SetWeight(wxFONTWEIGHT_NORMAL);

	wxClientDC dc(this);
	dc.SetFont(font);

	// Set line height
	lineHeight = dc.GetCharHeight() + 4;

	// Set row brushes
	row_colors.Default.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor()));
	row_colors.Header.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Header")->GetColor()));
	row_colors.Selection.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
	row_colors.Comment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Comment")->GetColor()));
	row_colors.Visible.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Inframe")->GetColor()));
	row_colors.SelectedComment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selected Comment")->GetColor()));
	row_colors.LeftCol.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Left Column")->GetColor()));

	SetColumnWidths();

	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::UpdateMaps() {
	index_line_map.clear();

	auto core = context->GetCore();
	for (auto& curdiag : core.ass->Events)
		index_line_map.push_back(&curdiag);

	SetColumnWidths();
	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::OnActiveLineChanged(AssDialogue *new_active) {
	if (new_active) {
		if (new_active->Row != active_row)
			MakeRowVisible(new_active->Row);
		extendRow = active_row = new_active->Row;
		Refresh(false);
	}
	else
		active_row = -1;
}

void BaseGrid::MakeRowVisible(int row) {
	int h = GetClientSize().GetHeight();

	if (row < yPos + 1)
		ScrollTo(row - 1);
	else if (row > yPos + h/lineHeight - 3)
		ScrollTo(row - h/lineHeight + 3);
}

void BaseGrid::SelectRow(int row, bool addToSelected, bool select) {
	if (row < 0 || (size_t)row >= index_line_map.size()) return;

	AssDialogue *line = index_line_map[row];
	auto core = context->GetCore();

	if (!addToSelected) {
		core.selectionController->SetSelectedSet(Selection{line});
		return;
	}

	bool selected = !!core.selectionController->GetSelectedSet().count(line);
	if (select != selected) {
		auto selection = core.selectionController->GetSelectedSet();
		if (select)
			selection.insert(line);
		else
			selection.erase(line);
		core.selectionController->SetSelectedSet(std::move(selection));
	}
}

void BaseGrid::OnCurrentFrameChanged(int frame_number) {
	current_frame = frame_number;
	auto new_visible_rows = GetRowsDisplayedAtCurrentFrame();
	if (new_visible_rows == visible_rows)
		return;

	RefreshChangedVisibleRows(visible_rows, new_visible_rows);
	visible_rows = std::move(new_visible_rows);
}

void BaseGrid::OnVideoProviderChanged() {
	auto core = context->GetCore();
	current_frame = core.project->VideoProvider() ? core.videoController->GetFrameN() : -1;
	Refresh(false);
}

void BaseGrid::OnIdle(wxIdleEvent&) {
	if (refresh_on_idle) {
		refresh_on_idle = false;
		Refresh(false);
	}
}

void BaseGrid::OnPaint(wxPaintEvent &) {
	int w = 0;
	int h = 0;
	GetClientSize(&w,&h);
	auto const layout = aegisub::grid::CalculateGridLayout({
		w,
		h,
		scrollBar->GetSize().GetWidth(),
		lineHeight,
		GetRows(),
		yPos
	});
	w = layout.grid_width;
	h = layout.client_height;
	const int nDraw = layout.rows_to_draw;

	// Find which columns and visible rows need to be repainted
	std::vector<char> paint_columns;
	paint_columns.resize(columns.size(), false);
	bool any = false;
	bool paint_header = false;
	int first_dirty_row = nDraw;
	int last_dirty_row = -1;
	for (wxRegionIterator region(GetUpdateRegion()); region; ++region) {
		wxRect updrect = region.GetRect();
		if (updrect.x < w && updrect.x + updrect.width > 0)
			any = true;
		if (updrect.y <= lineHeight && updrect.y + updrect.height > 0)
			paint_header = true;
		if (nDraw && updrect.y + updrect.height > lineHeight) {
			int first = updrect.y <= lineHeight ? 0 : (updrect.y - lineHeight) / lineHeight;
			int last = (updrect.y + updrect.height - 1 - lineHeight) / lineHeight;
			first_dirty_row = std::min(first_dirty_row, mid(0, first, nDraw - 1));
			last_dirty_row = std::max(last_dirty_row, mid(0, last, nDraw - 1));
		}

		int x = 0;
		for (size_t i : agi::util::range(columns.size())) {
			int width = columns[i]->Width();
			if (width && updrect.x < x + width && updrect.x + updrect.width > x) {
				paint_columns[i] = true;
			}
			x += width;
		}
	}

	if (!any) {
		wxBufferedPaintDC dc(this);
		dc.SetBackground(row_colors.Default);
		dc.Clear();
		return;
	}

	wxBufferedPaintDC dc(this);
	auto painter = MakeWxDcGridColumnPainter(dc);
	painter->SetFont(font);

	dc.SetBackground(row_colors.Default);
	dc.Clear();

	// Draw labels
	bool const has_dirty_rows = first_dirty_row <= last_dirty_row;
	if (has_dirty_rows) {
		int const top = (first_dirty_row + 1) * lineHeight;
		int const height = (last_dirty_row - first_dirty_row + 1) * lineHeight + 1;
		painter->FillRectangle(0, top, columns[0]->Width(), height,
			from_wx(row_colors.LeftCol.GetColour()));
	}

	// Row colors
	auto const text_standard = OPT_GET("Colour/Subtitle Grid/Standard")->GetColor();
	auto const text_selection = OPT_GET("Colour/Subtitle Grid/Selection")->GetColor();
	auto const text_collision = OPT_GET("Colour/Subtitle Grid/Collision")->GetColor();
	auto const grid_line_color = OPT_GET("Colour/Subtitle Grid/Lines")->GetColor();

	// First grid row
	if (paint_header)
		painter->DrawLine(0, 0, w, 0, grid_line_color);

	auto paint_text = [&](wxString const& str, int x, int y, int col) {
		int left = x + 4;
		if (columns[col]->Centered()) {
			int tw = 0, th = 0;
			painter->MeasureText(std::wstring(str.wx_str()), tw, th);
			left += (columns[col]->Width() - 6 - tw) / 2;
		}

		painter->DrawText(std::wstring(str.wx_str()), left, y + 2);
	};

	// Paint header
	if (paint_header) {
		painter->SetTextColor(text_standard);
		painter->FillRectangle(0, 0, w, lineHeight,
			from_wx(row_colors.Header.GetColour()));

		int x = 0;
		for (size_t i : agi::util::range(columns.size())) {
			if (paint_columns[i])
				paint_text(columns[i]->Header(), x, 0, i);
			x += columns[i]->Width();
		}

		painter->DrawLine(0, lineHeight, w, lineHeight, grid_line_color);
	}

	// Paint the rows
	const int grid_x = columns[0]->Width();

	auto core = context->GetCore();
	const auto active_line = core.selectionController->GetActiveLine();
	auto const& selection = core.selectionController->GetSelectedSet();
	visible_rows = GetRowsDisplayedAtCurrentFrame();

	for (int i = first_dirty_row; i <= last_dirty_row; ++i) {
		wxBrush color = row_colors.Default;
		AssDialogue *curDiag = index_line_map[i + yPos];

		bool inSel = !!selection.count(curDiag);
		if (inSel && curDiag->Comment)
			color = row_colors.SelectedComment;
		else if (inSel)
			color = row_colors.Selection;
		else if (curDiag->Comment)
			color = row_colors.Comment;

		if (std::binary_search(begin(visible_rows), end(visible_rows), i + yPos)) {
			if (color == row_colors.Default)
				color = row_colors.Visible;
		}
		auto const row_bg = from_wx(color.GetColour());
		painter->SetRowBackground(row_bg);

		// Draw row background color
		if (color != row_colors.Default)
			painter->FillRectangle(grid_x, (i + 1) * lineHeight + 1, w, lineHeight, row_bg);

		if (active_line != curDiag && curDiag->CollidesWith(active_line))
			painter->SetTextColor(text_collision);
		else if (inSel)
			painter->SetTextColor(text_selection);
		else
			painter->SetTextColor(text_standard);

		// Draw text
		int x = 0;
		int y = (i + 1) * lineHeight;
		for (size_t j : agi::util::range(columns.size())) {
			if (paint_columns[j])
				columns[j]->Paint(*painter, x, y, curDiag, context);
			x += columns[j]->Width();
		}

		// Draw grid
		painter->DrawLine(0, y, w, y, grid_line_color);
		painter->DrawLine(0, y + lineHeight, w , y + lineHeight, grid_line_color);
	}

	// Draw grid columns
	if (paint_header || has_dirty_rows) {
		int minH = paint_header ? 0 : (first_dirty_row + 1) * lineHeight;
		int maxH = has_dirty_rows ? (last_dirty_row + 2) * lineHeight : lineHeight;
		int x = 0;
		for (auto const& column : columns) {
			x += column->Width();
			if (x < w)
				painter->DrawLine(x, minH, x, maxH, grid_line_color);
		}
		painter->DrawLine(0, minH, 0, maxH, grid_line_color);
		painter->DrawLine(w, minH, w, maxH, grid_line_color);
	}

	int const active_screen_row = active_line ? active_line->Row - yPos : -1;
	if (active_screen_row >= first_dirty_row && active_screen_row <= last_dirty_row) {
		painter->StrokeRectangle(0, (active_screen_row + 1) * lineHeight, w, lineHeight + 1,
			OPT_GET("Colour/Subtitle Grid/Active Border")->GetColor());
	}
}

void BaseGrid::OnSize(wxSizeEvent &) {
	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::OnScroll(wxScrollEvent &event) {
	int newPos = event.GetPosition();
	if (yPos != newPos) {
		int old_y_pos = yPos;
		context->GetCore().ass->Properties.scroll_position = yPos = newPos;
		visible_rows = GetRowsDisplayedAtCurrentFrame();
		RefreshAfterScroll(old_y_pos);
	}
}

void BaseGrid::OnMouseEvent(wxMouseEvent &event) {
	int h = GetClientSize().GetHeight();
	bool shift = event.ShiftDown();
	bool alt = event.AltDown();
	bool ctrl = event.CmdDown();
	auto core = context->GetCore();
	auto ui = context->GetUI();

	// Row that mouse is over
	bool click = event.LeftDown();
	bool dclick = event.LeftDClick();
	int row = event.GetY() / lineHeight + yPos - 1;
	if (holding && !click)
		row = mid(0, row, GetRows()-1);
	AssDialogue *dlg = GetDialogue(row);
	if (!dlg) row = 0;

	if (event.ButtonDown() && OPT_GET("Subtitle/Grid/Focus Allow")->GetBool())
		SetFocus();

	if (holding) {
		if (!event.LeftIsDown()) {
			if (dlg)
				MakeRowVisible(row);
			holding = false;
			ReleaseMouse();
		}
		else {
			// Only scroll if the mouse has moved to a different row to avoid
			// scrolling on sloppy clicks
			if (row != extendRow) {
				if (row <= yPos)
					ScrollTo(yPos - 3);
				// When dragging down we give a 3 row margin to make it easier
				// to see what's going on, but we don't want to scroll down if
				// the user clicks on the bottom row and drags up
				else if (row > yPos + h / lineHeight - (row > extendRow ? 3 : 1))
					ScrollTo(yPos + 3);
			}
		}
	}
	else if (click && dlg) {
		holding = true;
		CaptureMouse();
	}

	if ((click || holding || dclick) && dlg) {
		int old_extend = extendRow;

		// SetActiveLine will scroll the grid if the row is only half-visible,
		// but we don't want to scroll until the mouse moves or the button is
		// released, to avoid selecting multiple lines on a click
		int old_y_pos = yPos;
		core.selectionController->SetActiveLine(dlg);
		ScrollTo(old_y_pos);
		extendRow = row;

		auto const& selection = core.selectionController->GetSelectedSet();

		// Toggle selected
		if (click && ctrl && !shift && !alt) {
			bool isSel = !!selection.count(dlg);
			if (isSel && selection.size() == 1) return;
			SelectRow(row, true, !isSel);
			return;
		}

		// Normal click
		if ((click || dclick) && !shift && !ctrl && !alt) {
			if (dclick) {
				if (ui.audioBox)
					ui.audioBox->ScrollToActiveLine();
				core.videoController->JumpToTime(dlg->Start);
			}
			SelectRow(row, false);
			return;
		}

		// Change active line only
		if (click && !shift && !ctrl && alt)
			return;

		// Block select
		if ((click && shift && !alt) || holding) {
			extendRow = old_extend;
			int i1 = row;
			int i2 = extendRow;

			if (i1 > i2)
				std::swap(i1, i2);

			// Toggle each
			Selection newsel;
			if (ctrl) newsel = selection;
			for (int i = i1; i <= i2; i++)
				newsel.insert(GetDialogue(i));
			core.selectionController->SetSelectedSet(std::move(newsel));
			return;
		}

		return;
	}

	// Mouse wheel
	if (event.GetWheelRotation() != 0) {
		if (ForwardMouseWheelEvent(this, event)) {
			int step = shift ? h / lineHeight - 2 : 3;
			ScrollTo(yPos - step * event.GetWheelRotation() / event.GetWheelDelta());
		}
		return;
	}

	event.Skip();
}

void BaseGrid::OnContextMenu(wxContextMenuEvent &evt) {
	wxPoint pos = evt.GetPosition();
	if (pos == wxDefaultPosition || ScreenToClient(pos).y > lineHeight) {
		if (!context_menu) context_menu = menu::GetMenu("grid_context", (wxID_HIGHEST + 1) + 8000, context);
		menu::OpenPopupMenu(context_menu.get(), this);
	}
	else {
		wxMenu menu;
		for (size_t i : agi::util::range(columns.size())) {
			if (columns[i]->CanHide())
				menu.Append(MENU_SHOW_COL + i, columns[i]->Description(), wxEmptyString, wxITEM_CHECK)->Check(columns[i]->Visible());
		}
		PopupMenu(&menu);
	}
}

void BaseGrid::ScrollTo(int y) {
	int nextY = mid(0, y, GetRows() - 1);
	if (yPos != nextY) {
		int old_y_pos = yPos;
		context->GetCore().ass->Properties.scroll_position = yPos = nextY;
		scrollBar->SetThumbPosition(yPos);
		visible_rows = GetRowsDisplayedAtCurrentFrame();
		RefreshAfterScroll(old_y_pos);
	}
}

std::vector<int> BaseGrid::GetRowsDisplayedAtCurrentFrame() const {
	std::vector<int> rows;
	if (!OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame")->GetBool())
		return rows;

	auto core = context->GetCore();
	if (!core.project->VideoProvider() || current_frame < 0)
		return rows;

	int lines = GetClientSize().GetHeight() / lineHeight + 1;
	lines = mid(0, lines, GetRows() - yPos);
	rows.reserve(lines);

	for (int i = yPos; i < yPos + lines; ++i) {
		if (IsDisplayed(index_line_map[i]))
			rows.push_back(i);
	}

	return rows;
}

wxRect BaseGrid::GetScrollableRect() const {
	int width = 0;
	int height = 0;
	GetClientSize(&width, &height);
	width -= scrollBar->GetSize().GetWidth();
	int top = lineHeight + 1;

	return wxRect(0, top, std::max(0, width), std::max(0, height - top));
}

void BaseGrid::RefreshChangedVisibleRows(std::vector<int> const& old_visible_rows, std::vector<int> const& new_visible_rows) {
	auto old_it = begin(old_visible_rows);
	auto new_it = begin(new_visible_rows);

	while (old_it != end(old_visible_rows) || new_it != end(new_visible_rows)) {
		int row = -1;
		if (old_it == end(old_visible_rows))
			row = *new_it++;
		else if (new_it == end(new_visible_rows))
			row = *old_it++;
		else if (*old_it < *new_it)
			row = *old_it++;
		else if (*new_it < *old_it)
			row = *new_it++;
		else {
			++old_it;
			++new_it;
			continue;
		}

		RefreshDialogueRow(GetDialogue(row));
	}
}

void BaseGrid::RefreshAfterScroll(int old_y_pos) {
	wxRect rect = GetScrollableRect();
	int delta_rows = yPos - old_y_pos;
	int delta_pixels = -delta_rows * lineHeight;

	if (rect.GetWidth() > 0 && rect.GetHeight() > 0 && delta_pixels > -rect.GetHeight() && delta_pixels < rect.GetHeight())
		ScrollWindow(0, delta_pixels, &rect);
	else
		Refresh(false);
}

void BaseGrid::AdjustScrollbar() {
	wxSize clientSize = GetClientSize();
	wxSize scrollbarSize = scrollBar->GetSize();

	scrollBar->Freeze();
	scrollBar->SetSize(clientSize.GetWidth() - scrollbarSize.GetWidth(), 0, scrollbarSize.GetWidth(), clientSize.GetHeight());

	if (GetRows() <= 1) {
		yPos = 0;
		scrollBar->Enable(false);
		scrollBar->Thaw();
		return;
	}

	if (!scrollBar->IsEnabled())
		scrollBar->Enable(true);

	int drawPerScreen = clientSize.GetHeight() / lineHeight;
	int rows = GetRows();

	context->GetCore().ass->Properties.scroll_position = yPos = mid(0, yPos, rows - 1);

	scrollBar->SetScrollbar(yPos, drawPerScreen, rows + drawPerScreen - 1, drawPerScreen - 2, true);
	scrollBar->Thaw();
}

void BaseGrid::RefreshDialogueRow(const AssDialogue *line) {
	if (!line)
		return;

	int const visible_row = line->Row - yPos;
	if (visible_row < 0)
		return;

	int width = 0;
	int height = 0;
	GetClientSize(&width, &height);

	int const top = (visible_row + 1) * lineHeight;
	if (top >= height)
		return;

	// Repaint the whole row so selection/background/border stay in sync with the edited text.
	RefreshRect(wxRect(0, top, width, lineHeight + 1), false);
}

void BaseGrid::SetColumnWidths() {
	int w, h;
	GetClientSize(&w, &h);

	// DC for text extents test
	wxClientDC dc(this);
	auto painter = MakeWxDcGridColumnPainter(dc);
	painter->SetFont(font);

	text_refresh_rects.clear();
	int x = 0;

	if (!width_helper)
		width_helper = agi::make_unique<WidthHelper>();
	width_helper->SetPainter(painter.get());

	for (auto const& column : columns) {
		column->UpdateWidth(context, *width_helper);
		if (column->Width() && column->RefreshOnTextChange())
			text_refresh_rects.emplace_back(x, 0, column->Width(), h);
		x += column->Width();
	}
	width_helper->Age();
}

AssDialogue *BaseGrid::GetDialogue(int n) const {
	if (static_cast<size_t>(n) >= index_line_map.size()) return nullptr;
	return index_line_map[n];
}

bool BaseGrid::IsDisplayed(const AssDialogue *line) const {
	auto core = context->GetCore();
	if (!core.project->VideoProvider() || current_frame < 0)
		return false;
	return core.project->Timecodes().FrameAtTime(line->Start, agi::vfr::START) <= current_frame
		&& core.project->Timecodes().FrameAtTime(line->End, agi::vfr::END) >= current_frame;
}

void BaseGrid::OnCharHook(wxKeyEvent &event) {
	if (hotkey::check("Subtitle Grid", context, event))
		return;

	int key = event.GetKeyCode();

	if (key == WXK_UP || key == WXK_DOWN ||
		key == WXK_PAGEUP || key == WXK_PAGEDOWN ||
		key == WXK_HOME || key == WXK_END)
	{
		event.Skip();
		return;
	}

	hotkey::check("Audio", context, event);
}

void BaseGrid::OnKeyDown(wxKeyEvent &event) {
	int w,h;
	GetClientSize(&w, &h);

	int key = event.GetKeyCode();
	bool ctrl = event.CmdDown();
	bool alt = event.AltDown();
	bool shift = event.ShiftDown();

	int dir = 0;
	int step = 1;
	if (key == WXK_UP) dir = -1;
	else if (key == WXK_DOWN) dir = 1;
	else if (key == WXK_PAGEUP) {
		dir = -1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_PAGEDOWN) {
		dir = 1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_HOME) {
		dir = -1;
		step = GetRows();
	}
	else if (key == WXK_END) {
		dir = 1;
		step = GetRows();
	}

	if (!dir) {
		event.Skip();
		return;
	}

	auto core = context->GetCore();
	auto active_line = core.selectionController->GetActiveLine();
	int old_extend = extendRow;
	int next = mid(0, (active_line ? active_line->Row : 0) + dir * step, GetRows() - 1);
	core.selectionController->SetActiveLine(GetDialogue(next));

	// Move selection
	if (!ctrl && !shift && !alt) {
		SelectRow(next);
		return;
	}

	// Move active only
	if (alt && !shift && !ctrl)
		return;

	// Shift-selection
	if (shift && !ctrl && !alt) {
		extendRow = old_extend;
		// Set range
		int begin = next;
		int end = extendRow;
		if (end < begin)
			std::swap(begin, end);

		// Select range
		Selection newsel;
		for (int i = begin; i <= end; i++)
			newsel.insert(GetDialogue(i));

		core.selectionController->SetSelectedSet(std::move(newsel));

		MakeRowVisible(next);
		return;
	}
}

void BaseGrid::SetByFrame(bool state) {
	SetDisplayMode(state ? SubtitleTimeDisplayMode::Frame : SubtitleTimeDisplayMode::Ass);
}

void BaseGrid::SetDisplayMode(SubtitleTimeDisplayMode mode) {
	if (display_mode == mode)
		return;

	if (mode == SubtitleTimeDisplayMode::Frame && !context->GetCore().project->Timecodes().IsLoaded())
		mode = SubtitleTimeDisplayMode::Ass;

	display_mode = mode;
	for (auto& column : columns)
		column->SetDisplayMode(display_mode);
	SetColumnWidths();
	Refresh(false);
}
