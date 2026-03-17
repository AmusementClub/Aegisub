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

#include "async_video_provider.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "export_fixstyle.h"
#include "include/aegisub/subtitles_provider.h"
#include "source_frame.h"
#include "subtitle_overlay.h"
#include "subtitle_overlay_blend.h"
#include "video_frame.h"
#include "video_provider_manager.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/make_unique.h>

enum {
	NEW_SUBS_FILE = -1,
	SUBS_FILE_ALREADY_LOADED = -2
};

namespace {
template<typename T>
std::shared_ptr<T> acquire_buffer(std::vector<std::shared_ptr<T>>& buffers) {
	for (auto& buffer : buffers) {
		if (buffer.use_count() == 1)
			return buffer;
	}

	auto buffer = std::make_shared<T>();
	buffers.push_back(buffer);
	return buffer;
}
}

VideoRenderPacket AsyncVideoProvider::ProcRenderPacket(int frame_number, double time, bool raw) {
	VideoRenderPacket packet;

	std::shared_ptr<VideoFrame> frame;
	frame = acquire_buffer(source_buffers);

	try {
		source_provider->GetFrame(frame_number, *frame);
	}
	catch (VideoProviderError const& err) { throw VideoProviderErrorEvent(err); }

	packet.source_frame_storage = frame;
	packet.source_frame = MakeSourceFrameView(*frame, source_provider->GetColorSpace());
	packet.time = time;

	if (raw || !subs_provider || !subs) {
		packet.composited_frame_storage = frame;
		return packet;
	}

	try {
		if (single_frame != frame_number && single_frame != SUBS_FILE_ALREADY_LOADED) {
			// Generally edits and seeks come in groups; if the last thing done
			// was seek it is more likely that the user will seek again and
			// vice versa. As such, if this is the first frame requested after
			// an edit, only export the currently visible lines (because the
			// other lines will probably not be viewed before the file changes
			// again), and if it's a different frame, export the entire file.
			if (single_frame != NEW_SUBS_FILE) {
				subs_provider->LoadSubtitles(subs.get());
				single_frame = SUBS_FILE_ALREADY_LOADED;
			}
			else {
				AssFixStylesFilter::ProcessSubs(subs.get());
				single_frame = frame_number;
				subs_provider->LoadSubtitles(subs.get(), time);
			}
		}
	}
	catch (agi::Exception const& err) { throw SubtitlesProviderErrorEvent(err.GetMessage()); }

	try {
		auto composited = acquire_buffer(composited_buffers);
		*composited = *frame;
		packet.composited_frame_storage = composited;

		if (subs_provider->GetRenderMode() == SubtitleRenderMode::PremultipliedOverlay) {
			auto overlay_storage = acquire_buffer(subtitle_overlay_buffers);
			overlay_storage->Reset(static_cast<int>(frame->width), static_cast<int>(frame->height), frame->flipped);
			auto subtitle_overlay = overlay_storage->MakeView(true);

			if (subs_provider->RenderOverlay(packet.source_frame, subtitle_overlay, time / 1000.)) {
				packet.subtitle_overlay_storage = overlay_storage;
				packet.subtitle_overlay = subtitle_overlay;
				packet.has_subtitle_overlay = true;
				CompositePremultipliedBgraOverlayOntoVideoFrame(*composited, subtitle_overlay);
			}
			else {
				subs_provider->DrawSubtitles(*composited, time / 1000.);
			}
		}
		else {
			subs_provider->DrawSubtitles(*composited, time / 1000.);
		}
	}
	catch (agi::UserCancelException const&) { }

	return packet;
}

static std::unique_ptr<SubtitlesProvider> get_subs_provider(wxEvtHandler *evt_handler, agi::BackgroundRunner *br) {
	try {
		return SubtitlesProviderFactory::GetProvider(br);
	}
	catch (agi::Exception const& err) {
		evt_handler->AddPendingEvent(SubtitlesProviderErrorEvent(err.GetMessage()));
		return nullptr;
	}
}

AsyncVideoProvider::AsyncVideoProvider(agi::fs::path const& video_filename, std::string const& colormatrix, wxEvtHandler *parent, agi::BackgroundRunner *br)
: AsyncVideoProvider(
	VideoProviderFactory::GetProvider(video_filename, colormatrix, br),
	get_subs_provider(parent, br),
	[parent](std::unique_ptr<wxEvent> evt) {
		if (parent)
			parent->QueueEvent(evt.release());
	})
{
}

AsyncVideoProvider::AsyncVideoProvider(std::unique_ptr<VideoProvider> source_provider, std::unique_ptr<SubtitlesProvider> subs_provider, AsyncVideoProviderEventSink event_sink)
: worker(agi::dispatch::Create())
, subs_provider(std::move(subs_provider))
, source_provider(std::move(source_provider))
, event_sink(std::move(event_sink))
{
}

AsyncVideoProvider::~AsyncVideoProvider() {
	worker->Sync([this] {
		while (ProcessPending()) { }
	});
}

void AsyncVideoProvider::LoadSubtitles(const AssFile *new_subs) throw() {
	auto copy = agi::make_unique<AssFile>(*new_subs);
	++content_version;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_subs = std::move(copy);
		pending_check_updated = false;
	}
	ScheduleProcessing();
}

void AsyncVideoProvider::UpdateSubtitles(const AssFile *new_subs, const AssDialogue *changed) throw() {
	(void)changed;
	auto copy = agi::make_unique<AssFile>(*new_subs);
	++content_version;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_subs = std::move(copy);
		if (!has_pending_frame)
			pending_check_updated = true;
	}
	ScheduleProcessing();
}

void AsyncVideoProvider::RequestFrame(int new_frame, double new_time) throw() {
	++request_version;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_time = new_time;
		pending_frame_number = new_frame;
		has_pending_frame = true;
		pending_check_updated = false;
	}
	ScheduleProcessing();
}

bool AsyncVideoProvider::NeedUpdate(std::vector<AssDialogueBase const*> const& visible_lines) {
	// Always need to render after a seek
	if (single_frame != NEW_SUBS_FILE || frame_number != last_rendered)
		return true;

	// Obviously need to render if the number of visible lines has changed
	if (visible_lines.size() != last_lines.size())
		return true;

	for (size_t i = 0; i < last_lines.size(); ++i) {
		auto const& last = last_lines[i];
		auto const& cur = *visible_lines[i];
		if (last.Layer  != cur.Layer)  return true;
		if (last.Margin != cur.Margin) return true;
		if (last.Style  != cur.Style)  return true;
		if (last.Effect != cur.Effect) return true;
		if (last.Text   != cur.Text)   return true;

		// Changing the start/end time effects the appearance only if the
		// line is animated. This is obviously not a very accurate check for
		// animated lines, but false positives aren't the end of the world
		if ((last.Start != cur.Start || last.End != cur.End) &&
			(!cur.Effect.get().empty() || cur.Text.get().find('\\') != std::string::npos))
			return true;
	}

	return false;
}

void AsyncVideoProvider::DeliverEvent(std::unique_ptr<wxEvent> evt) {
	if (event_sink)
		event_sink(std::move(evt));
}

void AsyncVideoProvider::ScheduleProcessing() {
	bool should_schedule = false;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!processing_scheduled) {
			processing_scheduled = true;
			should_schedule = true;
		}
	}

	if (!should_schedule)
		return;

	worker->Async([this] {
		while (ProcessPending()) { }
	});
}

bool AsyncVideoProvider::ProcessPending() {
	struct PendingWork {
		std::unique_ptr<AssFile> subs;
		bool check_updated = false;
		bool has_frame = false;
		int frame_number = -1;
		double time = -1.;
		bool has_color_space = false;
		std::string color_space;
		uint_fast32_t request_version = 0;
		uint_fast32_t content_version = 0;
	};

	PendingWork work;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!pending_subs && !has_pending_frame && !has_pending_color_space) {
			processing_scheduled = false;
			return false;
		}

		work.subs = std::move(pending_subs);
		work.check_updated = pending_check_updated;
		pending_check_updated = false;
		if (has_pending_frame) {
			work.has_frame = true;
			work.frame_number = pending_frame_number;
			work.time = pending_time;
			has_pending_frame = false;
		}
		else if (work.subs && frame_number >= 0) {
			work.has_frame = true;
			work.frame_number = frame_number;
			work.time = time;
		}
		if (has_pending_color_space) {
			work.has_color_space = true;
			work.color_space = pending_color_space;
			has_pending_color_space = false;
			pending_color_space.clear();
		}
		work.request_version = request_version.load(std::memory_order_relaxed);
		work.content_version = content_version.load(std::memory_order_relaxed);
	}

	if (work.has_color_space)
		source_provider->SetColorSpace(work.color_space);

	if (work.subs) {
		subs = std::move(work.subs);
		single_frame = NEW_SUBS_FILE;
	}

	if (!work.has_frame)
		return true;

	frame_number = work.frame_number;
	time = work.time;

	std::vector<AssDialogueBase const*> visible_lines;
	if (subs) {
		for (auto const& line : subs->Events) {
			if (!line.Comment && !(line.Start > time || line.End <= time))
				visible_lines.push_back(&line);
		}
	}

	if (work.check_updated && !NeedUpdate(visible_lines))
		return true;

	last_lines.clear();
	last_lines.reserve(visible_lines.size());
	for (auto line : visible_lines)
		last_lines.push_back(*line);
	last_rendered = frame_number;

	try {
		auto evt = std::make_unique<FrameReadyEvent>(ProcRenderPacket(frame_number, time), time);
		evt->SetEventType(EVT_FRAME_READY);
		auto current_content_version = content_version.load(std::memory_order_relaxed);
		auto current_request_version = request_version.load(std::memory_order_relaxed);
		bool should_deliver =
			work.content_version == current_content_version &&
			work.request_version == current_request_version;
		if (should_deliver)
			DeliverEvent(std::move(evt));
	}
	catch (wxEvent const& err) {
		auto current_content_version = content_version.load(std::memory_order_relaxed);
		auto current_request_version = request_version.load(std::memory_order_relaxed);
		bool should_deliver =
			work.content_version == current_content_version &&
			work.request_version == current_request_version;
		if (should_deliver)
			DeliverEvent(std::unique_ptr<wxEvent>(err.Clone()));
	}

	return true;
}

std::shared_ptr<VideoFrame> AsyncVideoProvider::GetFrame(int frame, double time, bool raw) {
	std::shared_ptr<VideoFrame> ret;
	worker->Sync([&]{
		while (ProcessPending()) { }
		ret = ProcRenderPacket(frame, time, raw).DisplayFrame();
	});
	return ret;
}

VideoRenderPacket AsyncVideoProvider::GetRenderPacket(int frame, double time, bool raw) {
	VideoRenderPacket ret;
	worker->Sync([&]{
		while (ProcessPending()) { }
		ret = ProcRenderPacket(frame, time, raw);
	});
	return ret;
}

void AsyncVideoProvider::SetColorSpace(std::string const& matrix) {
	++content_version;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_color_space = matrix;
		has_pending_color_space = true;
	}
	ScheduleProcessing();
}

wxDEFINE_EVENT(EVT_FRAME_READY, FrameReadyEvent);
wxDEFINE_EVENT(EVT_VIDEO_ERROR, VideoProviderErrorEvent);
wxDEFINE_EVENT(EVT_SUBTITLES_ERROR, SubtitlesProviderErrorEvent);

VideoProviderErrorEvent::VideoProviderErrorEvent(VideoProviderError const& err)
: agi::Exception(err.GetMessage())
{
	SetEventType(EVT_VIDEO_ERROR);
}
SubtitlesProviderErrorEvent::SubtitlesProviderErrorEvent(std::string const& err)
: agi::Exception(err)
{
	SetEventType(EVT_SUBTITLES_ERROR);
}
