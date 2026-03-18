// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include "include/aegisub/video_provider.h"
#include "source_frame_format_selection.h"
#include "video_render_packet.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>

#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <wx/event.h>

class AssDialogue;
class AssFile;
class SubtitlesProvider;
class VideoProvider;
class VideoProviderError;
struct AssDialogueBase;
struct VideoFrame;
namespace agi {
	class BackgroundRunner;
	namespace dispatch { class Queue; }
}

using AsyncVideoProviderEventSink = std::function<void(std::unique_ptr<wxEvent>)>;

/// A latest-only asynchronous helper for seek/drag preview requests.
///
/// Frame stepping in the editor uses a synchronous path in VideoController
/// because frame-by-frame inspection favors showing each intermediate result.
class AsyncVideoProvider {
	/// Asynchronous work queue
	std::unique_ptr<agi::dispatch::Queue> worker;

	/// Subtitles provider
	std::unique_ptr<SubtitlesProvider> subs_provider;
	/// Video provider
	std::unique_ptr<VideoProvider> source_provider;
	/// Event sink for frame-ready and error events
	AsyncVideoProviderEventSink event_sink;

	int frame_number = -1; ///< Last frame number requested
	double time = -1.; ///< Time of the frame to pass to the subtitle renderer

	/// Copy of the subtitles file to avoid having to touch the project context
	std::unique_ptr<AssFile> subs;

	/// If >= 0, the subtitles provider current has just the lines visible on
	/// that frame loaded. If -1, the entire file is loaded. If -2, the
	/// currently loaded file is out of date.
	int single_frame = -1;

	/// Last rendered frame number
	int last_rendered = -1;
	/// Last rendered subtitles on that frame
	std::vector<AssDialogueBase> last_lines;
	/// Check if we actually need to honor a frame request or if no visible
	/// lines have actually changed
	bool NeedUpdate(std::vector<AssDialogueBase const*> const& visible_lines);

	VideoRenderPacket ProcRenderPacket(int frame, double time, bool raw = false);

	/// Monotonic counter used to identify the latest seek/drag request.
	std::atomic<uint_fast32_t> request_version{ 0 };
	/// Monotonic counter used to invalidate frames when the rendered content changes.
	std::atomic<uint_fast32_t> content_version{ 0 };

	std::vector<std::shared_ptr<VideoFrame>> source_buffers;
	std::vector<std::shared_ptr<VideoFrame>> composited_buffers;
	std::vector<std::shared_ptr<SubtitleOverlayStorage>> subtitle_overlay_buffers;
	std::array<std::shared_ptr<SubtitleOverlayStorage>, 2> compatibility_overlay_buffers = { };
	int next_compatibility_overlay_buffer = 0;
	std::shared_ptr<SubtitleOverlayStorage> previous_compatibility_overlay;
	bool force_next_overlay_full_upload = false;

	std::mutex pending_mutex;
	std::unique_ptr<AssFile> pending_subs;
	bool pending_check_updated = false;
	bool has_pending_frame = false;
	int pending_frame_number = -1;
	double pending_time = -1.;
	bool has_pending_color_space = false;
	std::string pending_color_space;
	bool processing_scheduled = false;
	std::vector<SourceFramePixelFormat> preferred_source_formats = { SourceFramePixelFormat::Bgra8 };
	SourceFramePixelFormat selected_source_format = SourceFramePixelFormat::Bgra8;

	void DeliverEvent(std::unique_ptr<wxEvent> evt);
	void InvalidateOverlayPipelineState(bool force_full_upload);
	bool ReconfigureSourceOutputFormat();
	void ScheduleProcessing();
	bool ProcessPending();

public:
	/// @brief Load the passed subtitle file
	/// @param subs File to load
	///
	/// This function blocks until is it is safe for the calling thread to
	/// modify subs
	void LoadSubtitles(const AssFile *subs) throw();

	/// @brief Update a previously loaded subtitle file
	/// @param subs Subtitle file which was last passed to LoadSubtitles
	/// @param changes Set of lines which have changed
	///
	/// This function only supports changes to existing lines, and not
	/// insertions or deletions.
	void UpdateSubtitles(const AssFile *subs, const AssDialogue *changes) throw();

	/// @brief Queue a latest-only preview request for a frame
	/// @brief frame Frame number
	/// @brief time  Exact start time of the frame in seconds
	///
	/// This is intended for seek/drag preview. Pending requests are replaced by
	/// newer ones, so there is no guarantee that every requested frame is shown.
	void RequestFrame(int frame, double time) throw();

	/// @brief Synchronously get a frame
	/// @brief frame Frame number
	/// @brief time  Exact start time of the frame in seconds
	/// @brief raw   Get raw frame without subtitles
	std::shared_ptr<VideoFrame> GetFrame(int frame, double time, bool raw = false);
	VideoRenderPacket GetRenderPacket(int frame, double time, bool raw = false);

	/// Ask the video provider to change YCbCr matricies
	void SetColorSpace(std::string const& matrix);
	bool SetPreferredSourceFormats(std::vector<SourceFramePixelFormat> formats);
	void ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider> provider);
	SourceFramePixelFormat GetSelectedSourceFormat() const { return selected_source_format; }

	int GetFrameCount() const             { return source_provider->GetFrameCount(); }
	int GetWidth() const                  { return source_provider->GetWidth(); }
	int GetHeight() const                 { return source_provider->GetHeight(); }
	double GetDAR() const                 { return source_provider->GetDAR(); }
	agi::vfr::Framerate GetFPS() const    { return source_provider->GetFPS(); }
	std::vector<int> GetKeyFrames() const { return source_provider->GetKeyFrames(); }
	std::string GetColorSpace() const     { return source_provider->GetColorSpace(); }
	std::string GetRealColorSpace() const { return source_provider->GetRealColorSpace(); }
	std::string GetWarning() const        { return source_provider->GetWarning(); }
	std::string GetDecoderName() const    { return source_provider->GetDecoderName(); }
	bool ShouldSetVideoProperties() const { return source_provider->ShouldSetVideoProperties(); }
	bool HasAudio() const                 { return source_provider->HasAudio(); }

	/// @brief Constructor
	/// @param videoFileName File to open
	/// @param parent Event handler to send FrameReady events to
	AsyncVideoProvider(agi::fs::path const& filename, std::string const& colormatrix, wxEvtHandler *parent, agi::BackgroundRunner *br);
	AsyncVideoProvider(std::unique_ptr<VideoProvider> source_provider, std::unique_ptr<SubtitlesProvider> subs_provider, AsyncVideoProviderEventSink event_sink);
	~AsyncVideoProvider();
};

/// Event which signals that a requested frame is ready
struct FrameReadyEvent final : public wxEvent {
	VideoRenderPacket packet;
	/// Time which was used for subtitle rendering
	double time;
	wxEvent *Clone() const override { return new FrameReadyEvent(*this); };
	FrameReadyEvent(VideoRenderPacket packet, double time)
	: packet(std::move(packet)), time(time) { }
};

// These exceptions are wxEvents so that they can be passed directly back to
// the parent thread as events
struct VideoProviderErrorEvent final : public wxEvent, public agi::Exception {
	wxEvent *Clone() const override { return new VideoProviderErrorEvent(*this); };
	VideoProviderErrorEvent(VideoProviderError const& err);
};

struct SubtitlesProviderErrorEvent final : public wxEvent, public agi::Exception {
	wxEvent *Clone() const override { return new SubtitlesProviderErrorEvent(*this); };
	SubtitlesProviderErrorEvent(std::string const& msg);
};

wxDECLARE_EVENT(EVT_FRAME_READY, FrameReadyEvent);
wxDECLARE_EVENT(EVT_VIDEO_ERROR, VideoProviderErrorEvent);
wxDECLARE_EVENT(EVT_SUBTITLES_ERROR, SubtitlesProviderErrorEvent);
