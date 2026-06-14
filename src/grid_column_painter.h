// Copyright (c) 2026, MIRIMIR

#ifndef AEGISUB_GRID_COLUMN_PAINTER_H
#define AEGISUB_GRID_COLUMN_PAINTER_H

#include <memory>
#include <string>

namespace agi { struct Color; }
class wxDC;  // forward-declared; only the wxDC-backed factory references it

/// Abstraction over the per-cell paint operations BaseGrid needs.
///
/// This decouples GridColumn::Paint from a concrete drawing API, so the 
/// grid columns describe *what* to draw while the host (OnPaint) supplies 
/// the backend.
///
/// Coordinate space matches wxDC: text is positioned by its top-left corner
/// (not baseline), and MeasureText returns the same width/height wxDC would
/// report for the same string/font. This keeps the wxDC implementation a
/// pure pass-through and lets the D2D/DWrite implementation be tuned to match
/// GDI metrics exactly.
///
/// The painter is a *stateful* object, mirroring the wxDC state machine it
/// replaces: OnPaint sets the current text color and row-background color per
/// row before invoking the columns, and columns read them back where they
/// need to blend (see the CPS column). This preserves the original behavior
/// 1:1 with no per-call color plumbing through the column signatures.
class GridColumnPainter {
public:
	virtual ~GridColumnPainter() = default;

	/// Set the current grid font. Called once at the start of OnPaint / before
	/// any measurement pass.
	virtual void SetFont(class wxFont const& font) = 0;

	/// Measure the extent (width, height) of utf8 in the current grid font.
	/// Matches wxDC::GetTextExtent semantics: width is advance width, height
	/// is the font's line height. Out-params are untouched when text is empty.
	virtual void MeasureText(std::string const& utf8, int& out_width, int& out_height) = 0;
	virtual void MeasureText(std::wstring const& text, int& out_width, int& out_height) = 0;

	/// Draw text with its top-left at (x, y), using the current text color
	/// (see SetTextColor) and the current grid font.
	virtual void DrawText(std::string const& utf8, int x, int y) = 0;
	virtual void DrawText(std::wstring const& text, int x, int y) = 0;

	/// Solid fill (no border). Equivalent to SetBrush(color) + SetPen(*) +
	/// DrawRectangle on wxDC.
	virtual void FillRectangle(int x, int y, int w, int h, agi::Color const& color) = 0;

	/// 1px line. Equivalent to SetPen(color) + DrawLine on wxDC.
	virtual void DrawLine(int x0, int y0, int x1, int y1, agi::Color const& color) = 0;

	/// Rectangle outline (transparent fill). Equivalent to SetPen(color) +
	/// SetBrush(*wxTRANSPARENT_BRUSH) + DrawRectangle on wxDC. Used for the
	/// active-row border.
	virtual void StrokeRectangle(int x, int y, int w, int h, agi::Color const& color) = 0;

	/// Current text color (SetTextColor). The CPS column reads this to blend
	/// its warning-text color: blend(*wxBLACK, tc, alpha).
	virtual void SetTextColor(agi::Color const& color) = 0;
	virtual agi::Color CurrentTextColor() const = 0;

	/// Current row-background color. The OnPaint row loop sets this before
	/// invoking the column's Paint (it used to be dc.SetBrush(color)); the
	/// CPS column reads it back to blend its warning background.
	virtual void SetRowBackground(agi::Color const& color) = 0;
	virtual agi::Color CurrentRowBackground() const = 0;
};

/// Construct the wxDC-backed painter (the GDI / fallback path). The D2D path
/// lives behind WITH_D2D_GRID and provides its own factory.
std::unique_ptr<GridColumnPainter> MakeWxDcGridColumnPainter(wxDC& dc);

#endif // AEGISUB_GRID_COLUMN_PAINTER_H
