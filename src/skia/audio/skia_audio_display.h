#pragma once

#include "skia_audio_display_contract.h"
#include "skia_audio_frame_model.h"

#include <wx/glcanvas.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxEraseEvent;
class wxPaintEvent;
class wxMouseEvent;
class wxMouseCaptureLostEvent;
class wxFocusEvent;
class wxKeyEvent;
class wxSizeEvent;
class wxThreadEvent;
class wxTimerEvent;
class TimeRange;
class AudioController;

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
	void OnContentFailure(wxThreadEvent& event);
	void OnPresentationTimer(wxTimerEvent& event);
	void OnLoadTimer(wxTimerEvent& event);
	void OnMiddleSeekTimer(wxTimerEvent& event);
	void OnAudioOpen(agi::AudioProvider *provider);
	void OnPlaybackPosition(int position_ms);
	void OnPlaybackStop();
	void OnTimingControllerChanged();
	void OnTimingDataChanged();
	void OnMouseEvent(wxMouseEvent& event);
	void OnMouseEnter(wxMouseEvent& event);
	void OnMouseLeave(wxMouseEvent& event);
	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event);
	void OnFocus(wxFocusEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void EmitMiddleSeekOutput(int time_ms, bool commit);
	void ScheduleMiddleSeekTimer();
	void FinishMiddleSeek(int time_ms);
	void OnRenderingSettingsChanged();
	void ReconfigureAnalysis();
	void RebuildViewport();
	void RequestVisibleContent();
	bool HasCompleteVisibleContent() const;
	void CommitScrollbarContentViewport();
	void UpdatePresentationTiming();
	void RequestRepaint(bool interactive = false);
	void RequestFallback(std::string message);

public:
	using FailureCallback = std::function<void(std::string)>;

	SkiaAudioDisplay(
		wxWindow *parent,
		AudioController *controller,
		agi::Context *context,
		FailureInjection failure_injection,
		FailureCallback failure_callback);
	~SkiaAudioDisplay();

	SkiaAudioDisplay(SkiaAudioDisplay const&) = delete;
	SkiaAudioDisplay& operator=(SkiaAudioDisplay const&) = delete;

	void ClearFailureCallback();
	bool HasPresentedContentFrame() const noexcept;
	void SyncToCurrentAudioProvider();
	void ScrollBy(int pixel_amount);
	void ScrollBy(int pixel_amount, int mouse_x);
	void ScrollTimeRangeInView(TimeRange const& range);
	void SetZoomLevel(int zoom_level);
	int GetZoomLevel() const;
	void SetAmplitudeScale(float scale);
};

}
