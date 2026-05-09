// Copyright (c) 2005, Rodrigo Braz Monteiro
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

#include <chrono>
#include <vector>
#include <wx/timer.h>
#include <wx/window.h>

namespace agi { struct Context; }

class VideoController;
class AsyncVideoProvider;

/// @class VideoSlider
/// @brief Slider for displaying and adjusting the video position
class VideoSlider: public wxWindow {
	agi::Context *c; ///< Associated project context
	std::vector<int> keyframes; ///< Currently loaded keyframes
	std::vector<agi::signal::Connection> connections;

	bool is_dragging = false;

	int val = 0; ///< Current frame number
	int max = 1; ///< Last frame number

	wxTimer seek_timer;
	std::chrono::steady_clock::time_point last_seek_time;
	std::chrono::steady_clock::time_point pending_seek_deadline;
	int last_seek_frame = -1;
	int pending_seek_frame = -1;
	bool has_preview_seek_session = false;

	std::chrono::milliseconds seek_min_interval_forward{ 33 };
	std::chrono::milliseconds seek_min_interval_backward{ 100 };

	std::chrono::milliseconds GetSeekMinInterval(int target_frame) const;

	/// Get the frame number for the given x coordinate
	int GetValueAtX(int x);
	/// Get the x-coordinate for a frame number
	int GetXAtValue(int value);
	/// Set the position of the slider
	void SetValue(int value);
	void OnFramePresented(int value);

	/// Video open event handler
	void VideoOpened(AsyncVideoProvider *new_provider);
	/// Keyframe open even handler
	void KeyframesChanged(std::vector<int> const& newKeyframes);
	void UpdateScale();

	void ScheduleSeek(int target_frame, bool force);
	void OnSeekTimer(wxTimerEvent &evt);
	void OnMouse(wxMouseEvent &event);
	void OnKeyDown(wxKeyEvent &event);
	void OnCharHook(wxKeyEvent &event);
	void OnPaint(wxPaintEvent &);
	void OnFocus(wxFocusEvent &);

public:
	VideoSlider(wxWindow* parent, agi::Context *c);

	DECLARE_EVENT_TABLE()
};
