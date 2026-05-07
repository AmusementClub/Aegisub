// Copyright (c) 2026 MIRIMIRIM

#pragma once

#include <wx/stc/stc.h>

namespace aegisub::stc {

inline void ConfigureWindowsSelectionRendering(wxStyledTextCtrl *ctrl) {
#ifdef __WXMSW__
	// wxWidgets 3.3 enables a selection foreground by default. On MSW/GDI,
	// Scintilla then splits foreground drawing at selection boundaries, which
	// can isolate Common-script glyphs from CJK shaping context.
	ctrl->SetSelForeground(false, *wxBLACK);

	// Keep selected syntax-coloured text readable and visually close to the old
	// wxSTC/Scintilla default instead of wxWidgets 3.3's system highlight colour.
	ctrl->SetSelBackground(true, wxColour(0xC0, 0xC0, 0xC0));
	ctrl->SetAdditionalSelBackground(wxColour(0xD7, 0xD7, 0xD7));
#else
	wxUnusedVar(ctrl);
#endif
}

}
