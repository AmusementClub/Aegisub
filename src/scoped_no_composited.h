#pragma once

#ifdef _WIN32
#include <wx/sysopt.h>

// Keep the opt-out local to a window's creation, before wx chooses its class.
class ScopedWxNoComposited final {
	wxString previous;

	public:
	ScopedWxNoComposited()
		: previous(wxSystemOptions::GetOption(wxS("msw.window.no-composited"))) {
		wxSystemOptions::SetOption(wxS("msw.window.no-composited"), 1);
	}

	~ScopedWxNoComposited() {
		wxSystemOptions::SetOption(wxS("msw.window.no-composited"), previous);
	}

	ScopedWxNoComposited(ScopedWxNoComposited const&) = delete;
	ScopedWxNoComposited& operator=(ScopedWxNoComposited const&) = delete;
};
#endif
