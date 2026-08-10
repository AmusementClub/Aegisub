// Copyright (c) 2026, MIRIMIR

#include "grid_column_painter.h"

#include "compat.h"

#include <libaegisub/color.h>

#include <wx/brush.h>
#include <wx/dc.h>
#include <wx/font.h>
#include <wx/pen.h>
#include <wx/string.h>

namespace {

/// wxDC-backed GridColumnPainter. Pure pass-through -- every call maps 1:1
/// onto the wxDC operations BaseGrid::OnPaint used to issue directly. This is
/// the GDI path and the D2D fallback path.
class WxDcGridColumnPainter final : public GridColumnPainter {
	wxDC& dc;
	agi::Color text_color;
	agi::Color row_background;

public:
	explicit WxDcGridColumnPainter(wxDC& dc) : dc(dc) { }

	void SetFont(wxFont const& font) override { dc.SetFont(font); }
	void Clear(agi::Color const& color) override {
		dc.SetBackground(wxBrush(to_wx(color)));
		dc.Clear();
	}

	void MeasureText(std::string const& utf8, int& out_width, int& out_height) override {
		// wxString::FromUTF8 matches the historical non-Windows measure path;
		// the flyweight hot path uses the std::wstring overload instead.
		wxSize ext = dc.GetTextExtent(wxString::FromUTF8(utf8));
		out_width = ext.GetWidth();
		out_height = ext.GetHeight();
	}

	void MeasureText(std::wstring const& text, int& out_width, int& out_height) override {
		// wxString construction from wstring is a view on wchar_t platforms
		// (no allocation), which matters for the WidthHelper flyweight path.
		wxSize ext = dc.GetTextExtent(wxString(text));
		out_width = ext.GetWidth();
		out_height = ext.GetHeight();
	}

	void DrawText(std::string const& utf8, int x, int y) override {
		dc.DrawText(wxString::FromUTF8(utf8), x, y);
	}

	void DrawText(std::wstring const& text, int x, int y) override {
		dc.DrawText(wxString(text), x, y);
	}

	void FillRectangle(int x, int y, int w, int h, agi::Color const& color) override {
		dc.SetBrush(wxBrush(to_wx(color)));
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.DrawRectangle(x, y, w, h);
	}

	void DrawLine(int x0, int y0, int x1, int y1, agi::Color const& color) override {
		dc.SetPen(wxPen(to_wx(color)));
		dc.DrawLine(x0, y0, x1, y1);
	}

	void StrokeRectangle(int x, int y, int w, int h, agi::Color const& color) override {
		dc.SetPen(wxPen(to_wx(color)));
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		dc.DrawRectangle(x, y, w, h);
	}

	void SetTextColor(agi::Color const& color) override {
		text_color = color;
		dc.SetTextForeground(to_wx(color));
	}

	agi::Color CurrentTextColor() const override { return text_color; }

	void SetRowBackground(agi::Color const& color) override {
		row_background = color;
		// Mirror onto the dc too so any code path that still reads
		// dc.GetBrush().GetColour() directly (rather than via the painter)
		// keeps working. The painter state is the source of truth.
		dc.SetBrush(wxBrush(to_wx(color)));
	}

	agi::Color CurrentRowBackground() const override { return row_background; }
};

} // namespace

std::unique_ptr<GridColumnPainter> MakeWxDcGridColumnPainter(wxDC& dc) {
	return std::make_unique<WxDcGridColumnPainter>(dc);
}
