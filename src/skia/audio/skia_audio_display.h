#pragma once

#include "skia_audio_display_contract.h"

#include <wx/glcanvas.h>

#include <functional>
#include <memory>
#include <string>

class wxEraseEvent;
class wxPaintEvent;
class wxSizeEvent;
class wxThreadEvent;

namespace agi { class AudioProvider; struct Context; }

namespace aegisub::skia::audio {

// P3.3 visible sibling. It presents only a fixed diagnostic frame; real Audio
// frame models, content and interaction intentionally begin in later phases.
class SkiaAudioDisplay final : public wxGLCanvas {
	struct Impl;
	std::unique_ptr<Impl> impl;

	void OnPaint(wxPaintEvent& event);
	void OnEraseBackground(wxEraseEvent& event);
	void OnSize(wxSizeEvent& event);
	void OnContentReady(wxThreadEvent& event);
	void OnAudioOpen(agi::AudioProvider *provider);
	void RequestFallback(std::string message);

public:
	using FailureCallback = std::function<void(std::string)>;

	SkiaAudioDisplay(
		wxWindow *parent,
		agi::Context *context,
		FailureInjection failure_injection,
		FailureCallback failure_callback);
	~SkiaAudioDisplay();

	SkiaAudioDisplay(SkiaAudioDisplay const&) = delete;
	SkiaAudioDisplay& operator=(SkiaAudioDisplay const&) = delete;

	void ClearFailureCallback();
	void SyncToCurrentAudioProvider();
};

}
