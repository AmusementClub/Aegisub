// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "headless_cli_execute.h"
#include "headless_cli_internal.h"

#include <sstream>
#include <utility>

namespace headless_cli {
namespace {

std::string BuildMediaInspectJsonImpl(MediaInspectResult const& result) {
	auto build_track_choices_json = [](std::vector<aegisub::media_inspect_service::TrackChoiceInfo> const& tracks, int indent) {
		std::ostringstream out;
		std::string padding(indent, ' ');
		std::string entry_padding(indent + 2, ' ');
		out << "[\n";
		for (size_t i = 0; i < tracks.size(); ++i) {
			if (i != 0)
				out << ",\n";
			out << entry_padding
				<< "{ \"choice_index\": " << tracks[i].choice_index
				<< ", \"display_name\": \"" << detail::JsonEscape(tracks[i].display_name) << "\" }";
		}
		if (!tracks.empty())
			out << "\n";
		out << padding << "]";
		return out.str();
	};

	std::ostringstream out;
	out << "{\n";
	out << "  \"opened\": " << std::boolalpha << result.opened << ",\n";
	out << "  \"exit_code\": " << result.exit_code << ",\n";
	out << "  \"trace_dir\": \"" << detail::JsonEscape(detail::ToGenericString(result.trace_dir)) << "\",\n";
	out << "  \"message\": \"" << detail::JsonEscape(result.message) << "\",\n";
	out << "  \"selected_video_provider\": \"" << detail::JsonEscape(result.selected_video_provider) << "\",\n";
	out << "  \"selected_audio_provider\": \"" << detail::JsonEscape(result.selected_audio_provider) << "\",\n";
	out << "  \"actual_video_provider\": \"" << detail::JsonEscape(result.actual_video_provider) << "\",\n";
	out << "  \"actual_video_decoder\": \"" << detail::JsonEscape(result.actual_video_decoder) << "\",\n";
	out << "  \"video_provider_fallback\": " << result.video_provider_fallback << ",\n";
	out << "  \"video_provider_fallback_reason\": \"" << detail::JsonEscape(result.video_provider_fallback_reason) << "\",\n";
	out << "  \"video_provider_attempts\": \"" << detail::JsonEscape(result.video_provider_attempts) << "\",\n";
	out << "  \"actual_audio_provider_factory\": \"" << detail::JsonEscape(result.actual_audio_provider_factory) << "\",\n";
	out << "  \"actual_audio_provider\": \"" << detail::JsonEscape(result.actual_audio_provider) << "\",\n";
	out << "  \"audio_provider_fallback\": " << result.audio_provider_fallback << ",\n";
	out << "  \"audio_provider_fallback_reason\": \"" << detail::JsonEscape(result.audio_provider_fallback_reason) << "\",\n";
	out << "  \"audio_provider_attempts\": \"" << detail::JsonEscape(result.audio_provider_attempts) << "\",\n";
	out << "  \"video_track_choices\": " << build_track_choices_json(result.video_track_choices, 2) << ",\n";
	out << "  \"audio_track_choices\": " << build_track_choices_json(result.audio_track_choices, 2) << ",\n";
	out << "  \"subtitle_track_choices\": " << build_track_choices_json(result.subtitle_track_choices, 2) << ",\n";
	out << "  \"media\": {\n";
	out << "    \"video_path\": \"" << detail::JsonEscape(detail::ToGenericString(result.media.video_path)) << "\",\n";
	out << "    \"audio_path\": \"" << detail::JsonEscape(detail::ToGenericString(result.media.audio_path)) << "\",\n";
	out << "    \"has_video\": " << result.media.has_video << ",\n";
	out << "    \"has_audio\": " << result.media.has_audio << ",\n";
	out << "    \"can_load_subtitles_from_video\": " << result.media.can_load_subtitles_from_video << ",\n";
	out << "    \"video_width\": " << result.media.video_width << ",\n";
	out << "    \"video_height\": " << result.media.video_height << ",\n";
	out << "    \"video_frame_count\": " << result.media.video_frame_count << ",\n";
	out << "    \"video_duration_ms\": " << result.media.video_duration_ms << ",\n";
	out << "    \"video_decoder_name\": \"" << detail::JsonEscape(result.media.video_decoder_name) << "\",\n";
	out << "    \"audio_sample_rate\": " << result.media.audio_sample_rate << ",\n";
	out << "    \"audio_num_samples\": " << result.media.audio_num_samples << ",\n";
	out << "    \"audio_duration_ms\": " << result.media.audio_duration_ms << ",\n";
	out << "    \"audio_provider_name\": \"" << detail::JsonEscape(result.media.audio_provider_name) << "\"\n";
	out << "  },\n";
	out << "  \"playback\": {\n";
	out << "    \"has_video\": " << result.playback.has_video << ",\n";
	out << "    \"has_audio\": " << result.playback.has_audio << ",\n";
	out << "    \"video_playing\": " << result.playback.video_playing << ",\n";
	out << "    \"audio_playing\": " << result.playback.audio_playing << ",\n";
	out << "    \"playback_uses_audio_authority\": " << result.playback.playback_uses_audio_authority << ",\n";
	out << "    \"current_frame\": " << result.playback.current_frame << ",\n";
	out << "    \"current_video_time_ms\": " << result.playback.current_video_time_ms << ",\n";
	out << "    \"current_audio_time_ms\": " << result.playback.current_audio_time_ms << ",\n";
	out << "    \"primary_playback_begin_ms\": " << result.playback.primary_playback_begin_ms << ",\n";
	out << "    \"primary_playback_end_ms\": " << result.playback.primary_playback_end_ms << "\n";
	out << "  }\n";
	out << "}\n";
	return out.str();
}

std::string BuildAssInfoJsonImpl(AssInfoInspectResult const& result) {
	if (!result.snapshot) {
		return std::string("{\n  \"error\": \"") + detail::JsonEscape(result.error) + "\"\n}\n";
	}

	auto const& snapshot = *result.snapshot;
	std::ostringstream out;
	out << "{\n";
	out << "  \"subtitle_path\": \"" << detail::JsonEscape(detail::ToGenericString(snapshot.subtitle_path)) << "\",\n";
	out << "  \"format_name\": \"" << detail::JsonEscape(snapshot.format_name) << "\",\n";
	out << "  \"title\": \"" << detail::JsonEscape(snapshot.title) << "\",\n";
	out << "  \"script_type\": \"" << detail::JsonEscape(snapshot.script_type) << "\",\n";
	out << "  \"wrap_style\": \"" << detail::JsonEscape(snapshot.wrap_style) << "\",\n";
	out << "  \"scaled_border_and_shadow\": \"" << detail::JsonEscape(snapshot.scaled_border_and_shadow) << "\",\n";
	out << "  \"play_res_x\": " << snapshot.play_res_x << ",\n";
	out << "  \"play_res_y\": " << snapshot.play_res_y << ",\n";
	out << "  \"layout_res_x\": " << snapshot.layout_res_x << ",\n";
	out << "  \"layout_res_y\": " << snapshot.layout_res_y << ",\n";
	out << "  \"info_count\": " << snapshot.info_count << ",\n";
	out << "  \"style_count\": " << snapshot.style_count << ",\n";
	out << "  \"event_count\": " << snapshot.event_count << ",\n";
	out << "  \"dialogue_count\": " << snapshot.dialogue_count << ",\n";
	out << "  \"comment_count\": " << snapshot.comment_count << ",\n";
	out << "  \"attachment_count\": " << snapshot.attachment_count << ",\n";
	out << "  \"extradata_count\": " << snapshot.extradata_count << ",\n";
	out << "  \"project_audio_file\": \"" << detail::JsonEscape(snapshot.project_audio_file) << "\",\n";
	out << "  \"project_video_file\": \"" << detail::JsonEscape(snapshot.project_video_file) << "\",\n";
	out << "  \"project_timecodes_file\": \"" << detail::JsonEscape(snapshot.project_timecodes_file) << "\",\n";
	out << "  \"project_keyframes_file\": \"" << detail::JsonEscape(snapshot.project_keyframes_file) << "\"\n";
	out << "}\n";
	return out.str();
}

}

TraceInspectResult RunInspectTrace(TraceInspectRequest const& request) {
	TraceInspectResult result;
	auto service_result = aegisub::trace_inspect_service::Inspect(request);
	if (!service_result.session) {
		result.exit_code = 2;
		result.error = service_result.error;
		return result;
	}
	result.output = detail::BuildTraceInspectJson(*service_result.session);
	return result;
}

MediaInspectResult RunInspectMedia(MediaInspectRequest const& request) {
	return aegisub::media_inspect_service::Inspect(request);
}

AssInfoInspectResult RunInspectAssInfo(AssInfoInspectRequest const& request) {
	return aegisub::ass_info_service::Inspect(request);
}

std::string BuildMediaInspectJson(MediaInspectResult const& result) {
	return BuildMediaInspectJsonImpl(result);
}

std::string BuildAssInfoJson(AssInfoInspectResult const& result) {
	return BuildAssInfoJsonImpl(result);
}

void RunSessionPlaybackAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done) {
	aegisub::playback_session_service::RunAsync(std::move(request), std::move(on_done));
}

void RunSessionProjectAsync(ProjectSessionRequest request, std::function<void(ProjectSessionResult)> on_done) {
	aegisub::project_session_service::RunAsync(std::move(request), std::move(on_done));
}

void RunSessionAutomationAsync(AutomationSessionRequest request, std::function<void(AutomationSessionResult)> on_done) {
	aegisub::automation_session_service::RunAsync(std::move(request), std::move(on_done));
}

}
