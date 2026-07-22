#include "headless_service_output.h"

#include <libaegisub/fs.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace aegisub::headless_service_output {
namespace {

std::string JsonEscape(std::string const& input) {
	std::string escaped;
	escaped.reserve(input.size());
	for (unsigned char character : input) {
		switch (character) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (character < 0x20) {
					char buffer[7];
					snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(character));
					escaped += buffer;
				}
				else {
					escaped += static_cast<char>(character);
				}
				break;
		}
	}
	return escaped;
}

std::string GenericPath(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

bool LooksLikeJsonNumber(std::string const& value) {
	if (value.empty())
		return false;

	size_t index = value.front() == '-' ? 1 : 0;
	if (index == value.size())
		return false;

	if (value[index] == '0') {
		++index;
		if (index < value.size() && value[index] >= '0' && value[index] <= '9')
			return false;
	}
	else {
		if (value[index] < '1' || value[index] > '9')
			return false;
		while (index < value.size() && value[index] >= '0' && value[index] <= '9')
			++index;
	}

	if (index < value.size() && value[index] == '.') {
		++index;
		auto const fraction_start = index;
		while (index < value.size() && value[index] >= '0' && value[index] <= '9')
			++index;
		if (index == fraction_start)
			return false;
	}

	if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
		++index;
		if (index < value.size() && (value[index] == '+' || value[index] == '-'))
			++index;
		auto const exponent_start = index;
		while (index < value.size() && value[index] >= '0' && value[index] <= '9')
			++index;
		if (index == exponent_start)
			return false;
	}

	if (index != value.size())
		return false;

	char* end = nullptr;
	auto const parsed = std::strtod(value.c_str(), &end);
	return end && *end == '\0' && std::isfinite(parsed);
}

void AppendTypedJsonValue(std::ostringstream& output, std::string const& value) {
	if (value == "true" || value == "false" || LooksLikeJsonNumber(value))
		output << value;
	else
		output << '"' << JsonEscape(value) << '"';
}

std::string KeyValueMapToJson(std::map<std::string, std::string> const& values, int indent) {
	std::ostringstream output;
	auto const padding = std::string(indent, ' ');
	auto const entry_padding = std::string(indent + 2, ' ');
	output << "{\n";
	bool first = true;
	for (auto const& [key, value] : values) {
		if (!first) output << ",\n";
		first = false;
		output << entry_padding << '"' << JsonEscape(key) << "\": ";
		AppendTypedJsonValue(output, value);
	}
	if (!values.empty()) output << '\n';
	output << padding << '}';
	return output.str();
}

}

std::string BuildTraceInspectJson(trace_inspect_service::TraceSessionSummary const& session) {
	std::ostringstream output;
	output << "{\n";
	output << "  \"session_dir\": \"" << JsonEscape(GenericPath(session.session_dir)) << "\",\n";
	output << "  \"manifest\": " << KeyValueMapToJson(session.manifest, 2) << ",\n";
	output << "  \"summary\": " << KeyValueMapToJson(session.summary, 2) << "\n";
	output << "}\n";
	return output.str();
}

std::string BuildMediaInspectJson(media_inspect_service::MediaInspectResult const& result) {
	auto build_tracks = [](std::vector<media_inspect_service::TrackChoiceInfo> const& tracks, int indent) {
		std::ostringstream output;
		auto const padding = std::string(indent, ' ');
		auto const entry_padding = std::string(indent + 2, ' ');
		output << "[\n";
		for (size_t index = 0; index < tracks.size(); ++index) {
			if (index) output << ",\n";
			output << entry_padding << "{ \"choice_index\": " << tracks[index].choice_index
				<< ", \"display_name\": \"" << JsonEscape(tracks[index].display_name) << "\" }";
		}
		if (!tracks.empty()) output << '\n';
		output << padding << ']';
		return output.str();
	};

	std::ostringstream output;
	output << "{\n";
	output << "  \"opened\": " << std::boolalpha << result.opened << ",\n";
	output << "  \"exit_code\": " << result.exit_code << ",\n";
	output << "  \"trace_dir\": \"" << JsonEscape(GenericPath(result.trace_dir)) << "\",\n";
	output << "  \"message\": \"" << JsonEscape(result.message) << "\",\n";
	output << "  \"selected_video_provider\": \"" << JsonEscape(result.selected_video_provider) << "\",\n";
	output << "  \"selected_audio_provider\": \"" << JsonEscape(result.selected_audio_provider) << "\",\n";
	output << "  \"actual_video_provider\": \"" << JsonEscape(result.actual_video_provider) << "\",\n";
	output << "  \"actual_video_decoder\": \"" << JsonEscape(result.actual_video_decoder) << "\",\n";
	output << "  \"video_provider_fallback\": " << result.video_provider_fallback << ",\n";
	output << "  \"video_provider_fallback_reason\": \"" << JsonEscape(result.video_provider_fallback_reason) << "\",\n";
	output << "  \"video_provider_attempts\": \"" << JsonEscape(result.video_provider_attempts) << "\",\n";
	output << "  \"actual_audio_provider_factory\": \"" << JsonEscape(result.actual_audio_provider_factory) << "\",\n";
	output << "  \"actual_audio_provider\": \"" << JsonEscape(result.actual_audio_provider) << "\",\n";
	output << "  \"audio_provider_fallback\": " << result.audio_provider_fallback << ",\n";
	output << "  \"audio_provider_fallback_reason\": \"" << JsonEscape(result.audio_provider_fallback_reason) << "\",\n";
	output << "  \"audio_provider_attempts\": \"" << JsonEscape(result.audio_provider_attempts) << "\",\n";
	output << "  \"video_track_choices\": " << build_tracks(result.video_track_choices, 2) << ",\n";
	output << "  \"audio_track_choices\": " << build_tracks(result.audio_track_choices, 2) << ",\n";
	output << "  \"subtitle_track_choices\": " << build_tracks(result.subtitle_track_choices, 2) << ",\n";
	output << "  \"media\": {\n";
	output << "    \"video_path\": \"" << JsonEscape(GenericPath(result.media.video_path)) << "\",\n";
	output << "    \"audio_path\": \"" << JsonEscape(GenericPath(result.media.audio_path)) << "\",\n";
	output << "    \"has_video\": " << result.media.has_video << ",\n";
	output << "    \"has_audio\": " << result.media.has_audio << ",\n";
	output << "    \"can_load_subtitles_from_video\": " << result.media.can_load_subtitles_from_video << ",\n";
	output << "    \"video_width\": " << result.media.video_width << ",\n";
	output << "    \"video_height\": " << result.media.video_height << ",\n";
	output << "    \"video_frame_count\": " << result.media.video_frame_count << ",\n";
	output << "    \"video_duration_ms\": " << result.media.video_duration_ms << ",\n";
	output << "    \"video_decoder_name\": \"" << JsonEscape(result.media.video_decoder_name) << "\",\n";
	output << "    \"audio_sample_rate\": " << result.media.audio_sample_rate << ",\n";
	output << "    \"audio_num_samples\": " << result.media.audio_num_samples << ",\n";
	output << "    \"audio_duration_ms\": " << result.media.audio_duration_ms << ",\n";
	output << "    \"audio_provider_name\": \"" << JsonEscape(result.media.audio_provider_name) << "\"\n";
	output << "  },\n";
	output << "  \"playback\": {\n";
	output << "    \"has_video\": " << result.playback.has_video << ",\n";
	output << "    \"has_audio\": " << result.playback.has_audio << ",\n";
	output << "    \"video_playing\": " << result.playback.video_playing << ",\n";
	output << "    \"audio_playing\": " << result.playback.audio_playing << ",\n";
	output << "    \"playback_uses_audio_authority\": " << result.playback.playback_uses_audio_authority << ",\n";
	output << "    \"current_frame\": " << result.playback.current_frame << ",\n";
	output << "    \"current_video_time_ms\": " << result.playback.current_video_time_ms << ",\n";
	output << "    \"current_audio_time_ms\": " << result.playback.current_audio_time_ms << ",\n";
	output << "    \"primary_playback_begin_ms\": " << result.playback.primary_playback_begin_ms << ",\n";
	output << "    \"primary_playback_end_ms\": " << result.playback.primary_playback_end_ms << "\n";
	output << "  }\n";
	output << "}\n";
	return output.str();
}

std::string BuildAssInfoJson(ass_info_service::AssInfoInspectResult const& result) {
	if (!result.snapshot)
		return "{\n  \"error\": \"" + JsonEscape(result.error) + "\"\n}\n";
	auto const& snapshot = *result.snapshot;
	std::ostringstream output;
	output << "{\n";
	output << "  \"subtitle_path\": \"" << JsonEscape(GenericPath(snapshot.subtitle_path)) << "\",\n";
	output << "  \"format_name\": \"" << JsonEscape(snapshot.format_name) << "\",\n";
	output << "  \"title\": \"" << JsonEscape(snapshot.title) << "\",\n";
	output << "  \"script_type\": \"" << JsonEscape(snapshot.script_type) << "\",\n";
	output << "  \"wrap_style\": \"" << JsonEscape(snapshot.wrap_style) << "\",\n";
	output << "  \"scaled_border_and_shadow\": \"" << JsonEscape(snapshot.scaled_border_and_shadow) << "\",\n";
	output << "  \"play_res_x\": " << snapshot.play_res_x << ",\n";
	output << "  \"play_res_y\": " << snapshot.play_res_y << ",\n";
	output << "  \"layout_res_x\": " << snapshot.layout_res_x << ",\n";
	output << "  \"layout_res_y\": " << snapshot.layout_res_y << ",\n";
	output << "  \"info_count\": " << snapshot.info_count << ",\n";
	output << "  \"style_count\": " << snapshot.style_count << ",\n";
	output << "  \"event_count\": " << snapshot.event_count << ",\n";
	output << "  \"dialogue_count\": " << snapshot.dialogue_count << ",\n";
	output << "  \"comment_count\": " << snapshot.comment_count << ",\n";
	output << "  \"attachment_count\": " << snapshot.attachment_count << ",\n";
	output << "  \"extradata_count\": " << snapshot.extradata_count << ",\n";
	output << "  \"project_audio_file\": \"" << JsonEscape(snapshot.project_audio_file) << "\",\n";
	output << "  \"project_video_file\": \"" << JsonEscape(snapshot.project_video_file) << "\",\n";
	output << "  \"project_timecodes_file\": \"" << JsonEscape(snapshot.project_timecodes_file) << "\",\n";
	output << "  \"project_keyframes_file\": \"" << JsonEscape(snapshot.project_keyframes_file) << "\"\n";
	output << "}\n";
	return output.str();
}

}
