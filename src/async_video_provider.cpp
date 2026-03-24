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
#include "compatibility_overlay_buffer_plan.h"
#include "include/aegisub/subtitles_provider.h"
#include "source_frame.h"
#include "subtitle_overlay.h"
#include "subtitle_overlay_blend.h"
#include "video_frame.h"
#include "video_provider_manager.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

enum {
	NEW_SUBS_FILE = -1,
	SUBS_FILE_ALREADY_LOADED = -2
};

namespace {
constexpr int kCompatibilityOverlayTileSize = 64;
constexpr char const *kSourceModeLogTag = "video/source/mode";

std::string FormatSourceModeList(std::vector<SourceFrameOutputMode> const& modes) {
	std::string value = "[";
	for (size_t i = 0; i < modes.size(); ++i) {
		if (i)
			value.append(", ");
		value.append(SourceFrameOutputModeName(modes[i]));
	}
	value.push_back(']');
	return value;
}

char const *SourceFrameColorRangeName(SourceFrameColorRange range) {
	switch (range) {
		case SourceFrameColorRange::Limited:
			return "Limited";
		case SourceFrameColorRange::Full:
			return "Full";
		default:
			return "Unknown";
	}
}

std::string FormatColorMetadata(SourceFrameColorMetadata const& color) {
	return std::string("matrix=")
		+ (color.matrix.empty() ? "Unknown" : color.matrix)
		+ ", primaries="
		+ (color.primaries.empty() ? "Unknown" : color.primaries)
		+ ", transfer="
		+ (color.transfer.empty() ? "Unknown" : color.transfer)
		+ ", range="
		+ SourceFrameColorRangeName(color.range);
}

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

std::shared_ptr<SubtitleOverlayStorage> acquire_compatibility_overlay_buffer(
	std::array<std::shared_ptr<SubtitleOverlayStorage>, 2>& preferred_buffers,
	std::vector<std::shared_ptr<SubtitleOverlayStorage>>& overflow_buffers,
	std::shared_ptr<SubtitleOverlayStorage> const& previous_overlay,
	int& next_preferred_slot) {
	std::array<CompatibilityOverlayBufferSlotState, 2> slot_states = { };
	for (size_t i = 0; i < preferred_buffers.size(); ++i) {
		auto const& slot = preferred_buffers[i];
		slot_states[i].allocated = static_cast<bool>(slot);
		slot_states[i].reusable = slot && slot.use_count() == 1;
		slot_states[i].holds_previous = slot && slot.get() == previous_overlay.get();
	}

	auto plan = DecideCompatibilityOverlayBufferPlan(next_preferred_slot, slot_states);
	next_preferred_slot = plan.next_preferred_slot;

	if (plan.action == CompatibilityOverlayBufferPlanAction::UseOverflowPool)
		return acquire_buffer(overflow_buffers);

	size_t slot_index = plan.action == CompatibilityOverlayBufferPlanAction::UseSlot0 ? 0u : 1u;
	auto& slot = preferred_buffers[slot_index];
	if (!slot)
		slot = std::make_shared<SubtitleOverlayStorage>();
	return slot;
}
}

void AsyncVideoProvider::ResetCompatibilityOverlayState() {
	previous_compatibility_overlay.reset();
	next_compatibility_overlay_buffer = 0;
}

void AsyncVideoProvider::AdvanceOverlayContinuityGeneration() {
	++overlay_continuity_generation;
	if (overlay_continuity_generation == 0)
		++overlay_continuity_generation;
}

void AsyncVideoProvider::InvalidateProviderOverlayState() {
	if (subs_provider)
		subs_provider->InvalidateOverlayState();
	AdvanceOverlayContinuityGeneration();
}

VideoRenderPacket AsyncVideoProvider::ProcRenderPacket(int frame_number, double time, bool raw) {
	return ProcRenderPacket(frame_number, time, raw, false);
}

VideoRenderPacket AsyncVideoProvider::ProcRenderPacket(int frame_number, double time, bool raw, bool force_bgra_frame) {
	VideoRenderPacket packet;

	std::shared_ptr<VideoFrame> frame;
	bool native_frame_needs_display_transform_fallback = false;
	if (selected_source_mode == SourceFrameOutputMode::Native && !force_bgra_frame) {
		try {
			if (!source_provider->GetNativeFrame(frame_number, packet.source_frame, packet.source_frame_owner))
				throw VideoDecodeError("Selected native source mode but provider did not return a native frame.");
		}
		catch (VideoProviderError const& err) { throw VideoProviderErrorEvent(err); }
		if (!packet.source_frame.IsValid())
			throw VideoProviderErrorEvent(VideoDecodeError("Provider returned an invalid native source frame."));
		native_frame_needs_display_transform_fallback =
			SourceFrameNeedsDisplayTransformFallback(packet.source_frame);
	}
	else {
		frame = acquire_buffer(source_buffers);

		try {
			source_provider->GetFrame(frame_number, *frame);
		}
		catch (VideoProviderError const& err) { throw VideoProviderErrorEvent(err); }

		packet.source_frame_storage = frame;
		packet.source_frame_owner = frame;
		packet.source_frame = MakeSourceFrameView(*frame, source_provider->GetColorMetadata());
		packet.source_frame.geometry = source_provider->GetFrameGeometry();
		packet.source_frame.native_format = source_provider->GetNativeFormatIdentity();
	}
	packet.time = time;

	if (native_frame_needs_display_transform_fallback && !raw && subs_provider && subs) {
		frame = acquire_buffer(source_buffers);

		try {
			source_provider->GetFrame(frame_number, *frame);
		}
		catch (VideoProviderError const& err) { throw VideoProviderErrorEvent(err); }

		packet.source_frame_storage = frame;
		packet.source_frame_owner = frame;
		packet.source_frame = MakeBakedSourceFrameView(*frame, packet.source_frame);
		packet.source_frame.native_format = source_provider->GetNativeFormatIdentity();
	}

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
		std::shared_ptr<VideoFrame> composited;
		if (frame) {
			composited = acquire_buffer(composited_buffers);
			*composited = *frame;
			packet.composited_frame_storage = composited;
		}

		if (subs_provider->GetRenderMode() == SubtitleRenderMode::PremultipliedOverlay) {
			auto overlay_storage = acquire_buffer(subtitle_overlay_buffers);
			overlay_storage->Reset(
				packet.source_frame.width,
				packet.source_frame.height,
				packet.source_frame.flipped,
				!subs_provider->RenderOverlayClearsTarget());
			auto subtitle_overlay = overlay_storage->MakeView(true);

			if (subs_provider->RenderOverlay(packet.source_frame, subtitle_overlay, time / 1000.)) {
				overlay_storage->has_visible_content = subtitle_overlay.has_visible_content;
				if (subs_provider->SupportsOverlayDirtyRects()) {
					if (subtitle_overlay.dirty_rects && subtitle_overlay.dirty_rect_count > 0) {
						overlay_storage->dirty_rects.assign(
							subtitle_overlay.dirty_rects,
							subtitle_overlay.dirty_rects + subtitle_overlay.dirty_rect_count);
					}
					else {
						overlay_storage->dirty_rects.clear();
					}
				}
				else {
					overlay_storage->dirty_rects.clear();
					if (subtitle_overlay.has_visible_content
						&& subtitle_overlay.width > 0
						&& subtitle_overlay.height > 0) {
						overlay_storage->dirty_rects.push_back({
							subtitle_overlay.target_x,
							subtitle_overlay.target_y,
							subtitle_overlay.width,
							subtitle_overlay.height
						});
					}
				}
				subtitle_overlay.dirty_rects = overlay_storage->dirty_rects.empty()
					? nullptr
					: overlay_storage->dirty_rects.data();
				subtitle_overlay.dirty_rect_count = static_cast<int>(overlay_storage->dirty_rects.size());
				subtitle_overlay.continuity_generation = overlay_continuity_generation;
				if (subtitle_overlay.has_visible_content) {
					packet.subtitle_overlay_storage = overlay_storage;
					packet.subtitle_overlay = subtitle_overlay;
					packet.has_subtitle_overlay = true;
					if (composited)
						CompositePremultipliedBgraOverlayOntoVideoFrame(*composited, subtitle_overlay);
				}
			}
			else {
				if (!composited)
					throw SubtitlesProviderErrorEvent("Subtitle provider cannot bake subtitles into native source frames.");
				subs_provider->DrawSubtitles(*composited, time / 1000.);
			}
		}
		else {
			subs_provider->DrawSubtitles(*composited, time / 1000.);
			auto overlay_storage = acquire_compatibility_overlay_buffer(
				compatibility_overlay_buffers,
				subtitle_overlay_buffers,
				previous_compatibility_overlay,
				next_compatibility_overlay_buffer);
			SubtitleOverlay subtitle_overlay;
			if (BuildSparsePremultipliedCompatibilityOverlayWithDirtyTiles(
				*frame,
				*composited,
				previous_compatibility_overlay.get(),
				*overlay_storage,
				subtitle_overlay,
				kCompatibilityOverlayTileSize,
				kCompatibilityOverlayTileSize)) {
				bool should_emit_overlay =
					overlay_storage->has_visible_content ||
					(previous_compatibility_overlay && previous_compatibility_overlay->has_visible_content) ||
					!overlay_storage->dirty_rects.empty();
				if (should_emit_overlay) {
					subtitle_overlay = overlay_storage->MakeView(true);
					subtitle_overlay.color_role = SubtitleOverlayColorRole::SubtitleVideoCompatibility;
					subtitle_overlay.continuity_generation = overlay_continuity_generation;
					packet.subtitle_overlay_storage = overlay_storage;
					packet.subtitle_overlay = subtitle_overlay;
					packet.has_subtitle_overlay = true;
				}
				previous_compatibility_overlay = overlay_storage;
			}
			else {
				previous_compatibility_overlay.reset();
			}
		}
	}
	catch (agi::UserCancelException const&) { }

	return packet;
}

static std::unique_ptr<SubtitlesProvider> get_subs_provider(wxEvtHandler *evt_handler, agi::BackgroundRunner *br, std::shared_ptr<const TransientFontSet> transient_fonts, agi::ui::WeakLifetime event_lifetime) {
	try {
		return SubtitlesProviderFactory::GetProvider({ br, std::move(transient_fonts) });
	}
	catch (agi::Exception const& err) {
		if (!evt_handler)
			return nullptr;
		agi::ui::MainAsyncIfAlive(event_lifetime, [evt_handler, message = err.GetMessage()] {
			evt_handler->AddPendingEvent(SubtitlesProviderErrorEvent(message));
		});
		return nullptr;
	}
}

AsyncVideoProvider::AsyncVideoProvider(agi::fs::path const& video_filename, std::string const& colormatrix, wxEvtHandler *parent, agi::BackgroundRunner *br, std::shared_ptr<const TransientFontSet> transient_fonts, agi::ui::WeakLifetime event_lifetime, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink)
: AsyncVideoProvider(
	VideoProviderFactory::GetProvider(
		video_filename,
		colormatrix,
		br,
		std::move(choice_sink)),
	get_subs_provider(parent, br, std::move(transient_fonts), event_lifetime),
	[parent, event_lifetime](std::unique_ptr<wxEvent> evt) mutable {
		if (!parent)
			return;
		agi::ui::MainAsyncIfAlive(event_lifetime, [parent, evt = std::move(evt)]() mutable {
			parent->QueueEvent(evt.release());
		});
	})
{
}

AsyncVideoProvider::AsyncVideoProvider(std::unique_ptr<VideoProvider> source_provider, std::unique_ptr<SubtitlesProvider> subs_provider, AsyncVideoProviderEventSink event_sink)
: worker(agi::dispatch::Create())
, subs_provider(std::move(subs_provider))
, source_provider(std::move(source_provider))
, event_sink(std::move(event_sink))
{
	if (this->subs_provider)
		this->subs_provider->OnActivated();
	ReconfigureSourceOutputMode();
}

AsyncVideoProvider::~AsyncVideoProvider() {
	worker->Sync([this] {
		while (ProcessPending()) { }
	});
}

void AsyncVideoProvider::LoadSubtitles(const AssFile *new_subs) throw() {
	auto copy = agi::make_unique<AssFile>(*new_subs);
	++content_version;
	ResetCompatibilityOverlayState();
	InvalidateProviderOverlayState();
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
	ResetCompatibilityOverlayState();
	InvalidateProviderOverlayState();
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
		if (should_deliver) {
			DeliverEvent(std::move(evt));
		}
		else {
			ResetCompatibilityOverlayState();
			AdvanceOverlayContinuityGeneration();
		}
	}
	catch (wxEvent const& err) {
		auto current_content_version = content_version.load(std::memory_order_relaxed);
		auto current_request_version = request_version.load(std::memory_order_relaxed);
		bool should_deliver =
			work.content_version == current_content_version &&
			work.request_version == current_request_version;
		if (should_deliver)
			DeliverEvent(std::unique_ptr<wxEvent>(err.Clone()));
		else {
			ResetCompatibilityOverlayState();
			AdvanceOverlayContinuityGeneration();
		}
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

std::shared_ptr<VideoFrame> AsyncVideoProvider::GetFrameBgra(int frame, double time, bool raw) {
	std::shared_ptr<VideoFrame> ret;
	worker->Sync([&] {
		while (ProcessPending()) { }
		ret = ProcRenderPacket(frame, time, raw, true).DisplayFrame();
	});
	return ret;
}

bool AsyncVideoProvider::ReconfigureSourceOutputMode() {
	auto const compatibility_requires_bgra8 =
		subs_provider && subs_provider->GetRenderMode() == SubtitleRenderMode::CompatibilityFrameOnly;
	auto const available_modes = source_provider->GetAvailableSourceModes();
	auto const selected = SelectPreferredSourceFrameOutputMode(
		preferred_source_modes,
		available_modes,
		compatibility_requires_bgra8);

	auto applied = selected;
	if (!source_provider->SetOutputMode(applied)) {
		applied = SourceFrameOutputMode::Bgra8;
		if (!source_provider->SetOutputMode(applied))
			return false;
	}

	bool const mode_changed = selected_source_mode != applied;
	if (!has_logged_source_mode || mode_changed) {
		auto source_format = source_provider->GetNativeFormatDescription();
		auto render_color = source_provider->GetColorMetadata();
		auto source_color = source_provider->GetRealColorMetadata();
		LOG_I(kSourceModeLogTag)
			<< source_provider->GetDecoderName()
			<< ": preferred=" << FormatSourceModeList(preferred_source_modes)
			<< ", available=" << FormatSourceModeList(available_modes)
			<< ", selected=" << SourceFrameOutputModeName(selected)
			<< ", applied=" << SourceFrameOutputModeName(applied)
			<< ", source_format=" << (source_format.empty() ? "none" : source_format)
			<< ", render_color={" << FormatColorMetadata(render_color) << "}"
			<< ", source_color={" << FormatColorMetadata(source_color) << "}"
			<< (compatibility_requires_bgra8 ? ", compatibility_requires_bgra8=true" : "");
		has_logged_source_mode = true;
	}

	if (!mode_changed)
		return false;

	selected_source_mode = applied;
	++content_version;
	last_rendered = -1;
	last_lines.clear();
	ResetCompatibilityOverlayState();
	AdvanceOverlayContinuityGeneration();
	return true;
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
	if (subs_provider && subs_provider->GetRenderMode() == SubtitleRenderMode::CompatibilityFrameOnly) {
		ResetCompatibilityOverlayState();
		AdvanceOverlayContinuityGeneration();
	}
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_color_space = matrix;
		has_pending_color_space = true;
	}
	ScheduleProcessing();
}

bool AsyncVideoProvider::SetPreferredSourceModes(std::vector<SourceFrameOutputMode> modes) {
	if (modes.empty())
		modes.push_back(SourceFrameOutputMode::Bgra8);

	bool changed = false;
	worker->Sync([&] {
		while (ProcessPending()) { }
		preferred_source_modes = std::move(modes);
		changed = ReconfigureSourceOutputMode();
	});
	return changed;
}

void AsyncVideoProvider::ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider> provider) {
	worker->Sync([&] {
		while (ProcessPending()) { }
		auto old_provider = std::move(subs_provider);
		subs_provider = std::move(provider);
		if (subs_provider)
			subs_provider->OnActivated();
		old_provider.reset();
		bool const mode_changed = ReconfigureSourceOutputMode();
		single_frame = NEW_SUBS_FILE;
		last_rendered = -1;
		last_lines.clear();
		if (!mode_changed) {
			ResetCompatibilityOverlayState();
			AdvanceOverlayContinuityGeneration();
		}
	});
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
