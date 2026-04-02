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

#include "headless_cli_parse.h"
#include "headless_cli_internal.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace headless_cli {
namespace {

using detail::ParseAuthorityValue;
using detail::ParseBoolValue;
using detail::ParseIntegerValue;
using detail::RequireValue;
using detail::SplitWhitespace;
using detail::ToGenericString;
using detail::Trim;

bool ParseSessionStepLine(std::string const& line, size_t line_number, std::vector<aegisub::playback_session_service::PlaybackSessionStep>& steps, std::string& error) {
	using aegisub::playback_session_service::PlaybackAuthorityKind;
	using aegisub::playback_session_service::PlaybackSessionStep;
	using aegisub::playback_session_service::PlaybackSessionStepKind;

	auto const tokens = SplitWhitespace(line);
	if (tokens.empty())
		return true;

	auto invalid = [&](std::string const& message) {
		error = "session script line " + std::to_string(line_number) + ": " + message + " | " + line;
		return false;
	};

	auto require_int = [&](size_t index, char const* name) -> std::optional<int> {
		if (index >= tokens.size()) {
			error = "session script line " + std::to_string(line_number) + ": missing " + name + " | " + line;
			return std::nullopt;
		}
		auto parsed = ParseIntegerValue(tokens[index]);
		if (!parsed) {
			error = "session script line " + std::to_string(line_number) + ": invalid integer for " + name + " | " + line;
			return std::nullopt;
		}
		return parsed;
	};

	auto step = PlaybackSessionStep{};
	step.source_text = line;

	auto const& command = tokens[0];
	if (command == "open") {
		step.kind = PlaybackSessionStepKind::OpenMedia;
	}
	else if (command == "reopen") {
		step.kind = PlaybackSessionStepKind::ReopenMedia;
	}
	else if (command == "close") {
		step.kind = PlaybackSessionStepKind::CloseMedia;
	}
	else if (command == "install-playline") {
		if (tokens.size() != 3)
			return invalid("install-playline expects <start_ms> <duration_ms>");
		auto start_ms = require_int(1, "start_ms");
		auto duration_ms = require_int(2, "duration_ms");
		if (!start_ms || !duration_ms || *start_ms < 0 || *duration_ms <= 0)
			return invalid("install-playline requires non-negative start and positive duration");
		step.kind = PlaybackSessionStepKind::InstallPlayLine;
		step.primary_value = *start_ms;
		step.secondary_value = *duration_ms;
	}
	else if (command == "play") {
		step.kind = PlaybackSessionStepKind::PlayVideo;
	}
	else if (command == "playline") {
		step.kind = PlaybackSessionStepKind::PlayLine;
	}
	else if (command == "stop") {
		step.kind = PlaybackSessionStepKind::StopPlayback;
	}
	else if (command == "sleep") {
		if (tokens.size() != 2)
			return invalid("sleep expects <ms>");
		auto value = require_int(1, "ms");
		if (!value || *value < 0)
			return invalid("sleep requires a non-negative millisecond value");
		step.kind = PlaybackSessionStepKind::Sleep;
		step.primary_value = *value;
	}
	else if (command == "wait-playback-stop") {
		step.kind = PlaybackSessionStepKind::WaitPlaybackStop;
		if (tokens.size() > 2)
			return invalid("wait-playback-stop expects at most one timeout value");
		if (tokens.size() == 2) {
			auto value = require_int(1, "timeout_ms");
			if (!value || *value <= 0)
				return invalid("wait-playback-stop requires a positive timeout");
			step.primary_value = *value;
		}
		else {
			step.primary_value = 5000;
		}
	}
	else if (command == "jump-time") {
		if (tokens.size() != 2)
			return invalid("jump-time expects <ms>");
		auto value = require_int(1, "ms");
		if (!value || *value < 0)
			return invalid("jump-time requires a non-negative millisecond value");
		step.kind = PlaybackSessionStepKind::JumpToTime;
		step.primary_value = *value;
	}
	else if (command == "jump-frame") {
		if (tokens.size() != 2)
			return invalid("jump-frame expects <frame>");
		auto value = require_int(1, "frame");
		if (!value || *value < 0)
			return invalid("jump-frame requires a non-negative frame value");
		step.kind = PlaybackSessionStepKind::JumpToFrame;
		step.primary_value = *value;
	}
	else if (command == "query-media") {
		step.kind = PlaybackSessionStepKind::QueryMedia;
	}
	else if (command == "query-playback") {
		step.kind = PlaybackSessionStepKind::QueryPlayback;
	}
	else if (command == "assert-media") {
		if (tokens.size() != 3)
			return invalid("assert-media expects <has_video> <has_audio>");
		auto has_video = ParseBoolValue(tokens[1]);
		auto has_audio = ParseBoolValue(tokens[2]);
		if (!has_video || !has_audio)
			return invalid("assert-media expects true/false values");
		step.kind = PlaybackSessionStepKind::AssertMedia;
		step.expected_first = *has_video;
		step.expected_second = *has_audio;
	}
	else if (command == "assert-playing") {
		if (tokens.size() != 3)
			return invalid("assert-playing expects <video_playing> <audio_playing>");
		auto video_playing = ParseBoolValue(tokens[1]);
		auto audio_playing = ParseBoolValue(tokens[2]);
		if (!video_playing || !audio_playing)
			return invalid("assert-playing expects true/false values");
		step.kind = PlaybackSessionStepKind::AssertPlaying;
		step.expected_first = *video_playing;
		step.expected_second = *audio_playing;
	}
	else if (command == "assert-authority") {
		if (tokens.size() != 2)
			return invalid("assert-authority expects <audio|video>");
		auto authority = ParseAuthorityValue(tokens[1]);
		if (!authority)
			return invalid("assert-authority expects audio or video");
		step.kind = PlaybackSessionStepKind::AssertAuthority;
		step.expected_authority = *authority;
	}
	else {
		return invalid("unknown session step");
	}

	steps.push_back(std::move(step));
	return true;
}

bool ParseSessionScriptFile(agi::fs::path const& script_file, std::vector<aegisub::playback_session_service::PlaybackSessionStep>& steps, std::string& error) {
	std::ifstream in(script_file, std::ios::in);
	if (!in) {
		error = "could not open session script file: " + ToGenericString(script_file);
		return false;
	}

	std::string line;
	size_t line_number = 0;
	while (std::getline(in, line)) {
		++line_number;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto trimmed = Trim(line);
		if (trimmed.empty() || trimmed[0] == '#')
			continue;
		if (!ParseSessionStepLine(trimmed, line_number, steps, error))
			return false;
	}
	return true;
}

std::optional<PlaybackSessionRequest> ParseSessionPlaybackRequest(std::vector<std::string> const& args, std::string& error) {
	PlaybackSessionRequest request;
	agi::fs::path script_file;

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--script-file") {
			auto value = RequireValue(args, i, "--script-file", error);
			if (!value)
				return std::nullopt;
			script_file = agi::fs::path(*value);
			continue;
		}
		if (arg == "--video" || arg == "--probe-video") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.video_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--audio" || arg == "--probe-audio") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.audio_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--skip-audio" || arg == "--probe-skip-audio") {
			request.skip_audio = true;
			continue;
		}
		if (arg == "--video-provider" || arg == "--probe-video-provider") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.video_provider = *value;
			continue;
		}
		if (arg == "--audio-provider" || arg == "--probe-audio-provider") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.audio_provider = *value;
			continue;
		}
		if (arg == "--video-track-index" || arg == "--probe-video-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.video_track_index = *parsed;
			continue;
		}
		if (arg == "--audio-track-index" || arg == "--probe-audio-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.audio_track_index = *parsed;
			continue;
		}
		if (arg == "--trace-dir" || arg == "--probe-trace-dir") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.trace_dir = agi::fs::path(*value);
			continue;
		}
		if (arg == "--audio-rate-scale" || arg == "--probe-audio-rate-scale") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			try {
				request.audio_rate_scale = std::stod(*value);
			}
			catch (...) {
				error = arg + " requires a positive number\n" + Usage();
				return std::nullopt;
			}
			if (request.audio_rate_scale <= 0.0) {
				error = arg + " requires a positive number\n" + Usage();
				return std::nullopt;
			}
			continue;
		}
		if (arg == "--audio-quantum-ms" || arg == "--probe-audio-quantum-ms") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.audio_quantum_ms = *parsed;
			continue;
		}

		error = "unrecognized session playback argument: " + arg + "\n" + Usage();
		return std::nullopt;
	}

	if (script_file.empty()) {
		error = "--cli session playback requires --script-file\n" + Usage();
		return std::nullopt;
	}
	if (!agi::fs::FileExists(script_file)) {
		error = "session playback script file does not exist: " + ToGenericString(script_file);
		return std::nullopt;
	}
	if (request.video_path.empty() && request.audio_path.empty()) {
		error = "--cli session playback requires --video or --audio\n" + Usage();
		return std::nullopt;
	}
	if (!ParseSessionScriptFile(script_file, request.steps, error)) {
		error += "\n" + Usage();
		return std::nullopt;
	}
	if (request.steps.empty()) {
		error = "session playback script is empty: " + ToGenericString(script_file);
		return std::nullopt;
	}
	if (!request.skip_audio && request.audio_path.empty())
		request.audio_path = request.video_path;

	return request;
}

bool ParseProjectSessionStepLine(std::string const& line, size_t line_number, std::vector<aegisub::project_session_service::ProjectSessionStep>& steps, std::string& error) {
	using aegisub::project_session_service::ProjectSessionStep;
	using aegisub::project_session_service::ProjectSessionStepKind;

	auto const tokens = SplitWhitespace(line);
	if (tokens.empty())
		return true;

	auto invalid = [&](std::string const& message) {
		error = "project session script line " + std::to_string(line_number) + ": " + message + " | " + line;
		return false;
	};

	auto require_int = [&](size_t index, char const* name) -> std::optional<int> {
		if (index >= tokens.size()) {
			error = "project session script line " + std::to_string(line_number) + ": missing " + name + " | " + line;
			return std::nullopt;
		}
		auto parsed = ParseIntegerValue(tokens[index]);
		if (!parsed) {
			error = "project session script line " + std::to_string(line_number) + ": invalid integer for " + name + " | " + line;
			return std::nullopt;
		}
		return parsed;
	};

	auto remainder_after_token = [&](size_t token_count) -> std::string {
		size_t index = 0;
		size_t consumed_tokens = 0;
		while (index < line.size() && consumed_tokens < token_count) {
			while (index < line.size() && !std::isspace(static_cast<unsigned char>(line[index])))
				++index;
			++consumed_tokens;
			while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])))
				++index;
		}
		return index < line.size() ? line.substr(index) : std::string{};
	};

	auto step = ProjectSessionStep{};
	step.source_text = line;

	auto const& command = tokens[0];
	if (command == "open-media") {
		step.kind = ProjectSessionStepKind::OpenMedia;
	}
	else if (command == "reopen-media") {
		step.kind = ProjectSessionStepKind::ReopenMedia;
	}
	else if (command == "close-media") {
		step.kind = ProjectSessionStepKind::CloseMedia;
	}
	else if (command == "open-subtitles") {
		step.kind = ProjectSessionStepKind::OpenSubtitles;
	}
	else if (command == "open-subtitles-unlinked") {
		step.kind = ProjectSessionStepKind::OpenSubtitlesUnlinked;
	}
	else if (command == "open-subtitles-from-video") {
		step.kind = ProjectSessionStepKind::OpenSubtitlesFromVideo;
	}
	else if (command == "close-subtitles") {
		step.kind = ProjectSessionStepKind::CloseSubtitles;
	}
	else if (command == "open-timecodes") {
		step.kind = ProjectSessionStepKind::OpenTimecodes;
	}
	else if (command == "close-timecodes") {
		step.kind = ProjectSessionStepKind::CloseTimecodes;
	}
	else if (command == "open-keyframes") {
		step.kind = ProjectSessionStepKind::OpenKeyframes;
	}
	else if (command == "close-keyframes") {
		step.kind = ProjectSessionStepKind::CloseKeyframes;
	}
	else if (command == "insert-dialogue") {
		if (tokens.size() < 5)
			return invalid("insert-dialogue expects <row> <start_ms> <end_ms> <comment> [text...]");
		auto row = require_int(1, "row");
		auto start_ms = require_int(2, "start_ms");
		auto end_ms = require_int(3, "end_ms");
		auto comment = ParseBoolValue(tokens[4]);
		if (!row || !start_ms || !end_ms || !comment || *row < 0 || *start_ms < 0 || *end_ms <= *start_ms)
			return invalid("insert-dialogue requires non-negative row/start, end > start, and true/false comment");
		step.kind = ProjectSessionStepKind::InsertDialogue;
		step.primary_value = *row;
		step.secondary_value = *start_ms;
		step.tertiary_value = *end_ms;
		step.bool_value = *comment;
		step.text_value = remainder_after_token(5);
	}
	else if (command == "delete-dialogue") {
		if (tokens.size() != 2)
			return invalid("delete-dialogue expects <row>");
		auto row = require_int(1, "row");
		if (!row || *row < 0)
			return invalid("delete-dialogue requires a non-negative row");
		step.kind = ProjectSessionStepKind::DeleteDialogue;
		step.primary_value = *row;
	}
	else if (command == "set-dialogue-times") {
		if (tokens.size() != 4)
			return invalid("set-dialogue-times expects <row> <start_ms> <end_ms>");
		auto row = require_int(1, "row");
		auto start_ms = require_int(2, "start_ms");
		auto end_ms = require_int(3, "end_ms");
		if (!row || !start_ms || !end_ms || *row < 0 || *start_ms < 0 || *end_ms <= *start_ms)
			return invalid("set-dialogue-times requires non-negative row/start and end > start");
		step.kind = ProjectSessionStepKind::SetDialogueTimes;
		step.primary_value = *row;
		step.secondary_value = *start_ms;
		step.tertiary_value = *end_ms;
	}
	else if (command == "assert-dialogue") {
		if (tokens.size() < 5)
			return invalid("assert-dialogue expects <row> <start_ms> <end_ms> <comment> [text...]");
		auto row = require_int(1, "row");
		auto start_ms = require_int(2, "start_ms");
		auto end_ms = require_int(3, "end_ms");
		auto comment = ParseBoolValue(tokens[4]);
		if (!row || !start_ms || !end_ms || !comment || *row < 0 || *start_ms < 0 || *end_ms <= *start_ms)
			return invalid("assert-dialogue requires non-negative row/start, end > start, and true/false comment");
		step.kind = ProjectSessionStepKind::AssertDialogue;
		step.primary_value = *row;
		step.secondary_value = *start_ms;
		step.tertiary_value = *end_ms;
		step.bool_value = *comment;
		step.text_value = remainder_after_token(5);
	}
	else if (command == "save-subtitles") {
		if (tokens.size() != 1)
			return invalid("save-subtitles does not accept inline arguments; use --output-subtitle when needed");
		step.kind = ProjectSessionStepKind::SaveSubtitles;
	}
	else if (command == "query-project") {
		step.kind = ProjectSessionStepKind::QueryProject;
	}
	else if (command == "assert-project") {
		if (tokens.size() != 6)
			return invalid("assert-project expects <subtitle_file_loaded> <has_video> <has_audio> <timecodes_file_loaded> <keyframes_file_loaded>");
		auto subtitle_loaded = ParseBoolValue(tokens[1]);
		auto has_video = ParseBoolValue(tokens[2]);
		auto has_audio = ParseBoolValue(tokens[3]);
		auto timecodes_loaded = ParseBoolValue(tokens[4]);
		auto keyframes_loaded = ParseBoolValue(tokens[5]);
		if (!subtitle_loaded || !has_video || !has_audio || !timecodes_loaded || !keyframes_loaded)
			return invalid("assert-project expects true/false values");
		step.kind = ProjectSessionStepKind::AssertProject;
		step.expected_subtitle_file_loaded = *subtitle_loaded;
		step.expected_has_video = *has_video;
		step.expected_has_audio = *has_audio;
		step.expected_timecodes_file_loaded = *timecodes_loaded;
		step.expected_keyframes_file_loaded = *keyframes_loaded;
	}
	else if (command == "assert-subtitle-counts") {
		if (tokens.size() != 5)
			return invalid("assert-subtitle-counts expects <style_count> <event_count> <dialogue_count> <comment_count>");
		auto style_count = require_int(1, "style_count");
		auto event_count = require_int(2, "event_count");
		auto dialogue_count = require_int(3, "dialogue_count");
		auto comment_count = require_int(4, "comment_count");
		if (!style_count || !event_count || !dialogue_count || !comment_count
			|| *style_count < 0 || *event_count < 0 || *dialogue_count < 0 || *comment_count < 0)
			return invalid("assert-subtitle-counts requires non-negative integers");
		step.kind = ProjectSessionStepKind::AssertSubtitleCounts;
		step.primary_value = *style_count;
		step.secondary_value = *event_count;
		step.tertiary_value = *dialogue_count;
		step.quaternary_value = *comment_count;
	}
	else if (command == "assert-subtitle-modified") {
		if (tokens.size() != 2)
			return invalid("assert-subtitle-modified expects <true|false>");
		auto modified = ParseBoolValue(tokens[1]);
		if (!modified)
			return invalid("assert-subtitle-modified expects true or false");
		step.kind = ProjectSessionStepKind::AssertSubtitleModified;
		step.expected_subtitle_modified = *modified;
	}
	else {
		return invalid("unknown project session step");
	}

	steps.push_back(std::move(step));
	return true;
}

bool ParseProjectSessionScriptFile(agi::fs::path const& script_file, std::vector<aegisub::project_session_service::ProjectSessionStep>& steps, std::string& error) {
	std::ifstream in(script_file, std::ios::in);
	if (!in) {
		error = "could not open project session script file: " + ToGenericString(script_file);
		return false;
	}

	std::string line;
	size_t line_number = 0;
	while (std::getline(in, line)) {
		++line_number;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto trimmed = Trim(line);
		if (trimmed.empty() || trimmed[0] == '#')
			continue;
		if (!ParseProjectSessionStepLine(trimmed, line_number, steps, error))
			return false;
	}
	return true;
}

std::optional<ProjectSessionRequest> ParseSessionProjectRequest(std::vector<std::string> const& args, std::string& error) {
	ProjectSessionRequest request;
	agi::fs::path script_file;

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--script-file") {
			auto value = RequireValue(args, i, "--script-file", error);
			if (!value)
				return std::nullopt;
			script_file = agi::fs::path(*value);
			continue;
		}
		if (arg == "--video") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.video_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--audio") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.audio_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--subtitle") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.subtitle_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--output-subtitle") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.output_subtitle_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--subtitle-encoding") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.subtitle_encoding = *value;
			continue;
		}
		if (arg == "--timecodes") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.timecodes_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--keyframes") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.keyframes_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--skip-audio") {
			request.skip_audio = true;
			continue;
		}
		if (arg == "--video-provider") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.video_provider = *value;
			continue;
		}
		if (arg == "--audio-provider") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.audio_provider = *value;
			continue;
		}
		if (arg == "--video-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.video_track_index = *parsed;
			continue;
		}
		if (arg == "--audio-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.audio_track_index = *parsed;
			continue;
		}
		if (arg == "--subtitle-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.subtitle_track_index = *parsed;
			continue;
		}
		if (arg == "--trace-dir") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.trace_dir = agi::fs::path(*value);
			continue;
		}
		if (arg == "--audio-rate-scale") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			try {
				request.audio_rate_scale = std::stod(*value);
			}
			catch (...) {
				error = arg + " requires a positive number\n" + Usage();
				return std::nullopt;
			}
			if (request.audio_rate_scale <= 0.0) {
				error = arg + " requires a positive number\n" + Usage();
				return std::nullopt;
			}
			continue;
		}
		if (arg == "--audio-quantum-ms") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.audio_quantum_ms = *parsed;
			continue;
		}

		error = "unrecognized session project argument: " + arg + "\n" + Usage();
		return std::nullopt;
	}

	if (script_file.empty()) {
		error = "--cli session project requires --script-file\n" + Usage();
		return std::nullopt;
	}
	if (!agi::fs::FileExists(script_file)) {
		error = "session project script file does not exist: " + ToGenericString(script_file);
		return std::nullopt;
	}
	if (!ParseProjectSessionScriptFile(script_file, request.steps, error)) {
		error += "\n" + Usage();
		return std::nullopt;
	}
	if (request.steps.empty()) {
		error = "session project script is empty: " + ToGenericString(script_file);
		return std::nullopt;
	}
	if (!request.skip_audio && request.audio_path.empty() && !request.video_path.empty())
		request.audio_path = request.video_path;

	return request;
}

std::vector<agi::fs::path> ReadPathListFile(agi::fs::path const& list_file) {
	std::ifstream in(list_file, std::ios::in);
	std::vector<agi::fs::path> paths;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		line = Trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		paths.emplace_back(line);
	}
	return paths;
}

std::vector<agi::fs::path> ResolveBatchInputs(std::vector<agi::fs::path> direct_inputs, agi::fs::path const& list_file) {
	if (!list_file.empty()) {
		auto listed = ReadPathListFile(list_file);
		direct_inputs.insert(direct_inputs.end(), listed.begin(), listed.end());
	}
	return direct_inputs;
}

std::optional<MediaInspectRequest> ParseInspectMediaRequest(std::vector<std::string> const& args, std::string& error) {
	MediaInspectRequest request;

	if (args.size() == 5 && !args[4].empty() && args[4][0] != '-') {
		request.video_path = agi::fs::path(args[4]);
		request.audio_path = request.video_path;
		return request;
	}

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--video" || arg == "--probe-video") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.video_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--audio" || arg == "--probe-audio") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.audio_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--skip-audio" || arg == "--probe-skip-audio") {
			request.skip_audio = true;
			continue;
		}
		if (arg == "--video-provider" || arg == "--probe-video-provider") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.video_provider = *value;
			continue;
		}
		if (arg == "--audio-provider" || arg == "--probe-audio-provider") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.audio_provider = *value;
			continue;
		}
		if (arg == "--video-track-index" || arg == "--probe-video-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.video_track_index = *parsed;
			continue;
		}
		if (arg == "--audio-track-index" || arg == "--probe-audio-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.audio_track_index = *parsed;
			continue;
		}
		if (arg == "--subtitle-track-index" || arg == "--probe-subtitle-track-index") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.subtitle_track_index = *parsed;
			continue;
		}
		if (arg == "--trace-dir" || arg == "--probe-trace-dir") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.trace_dir = agi::fs::path(*value);
			continue;
		}
		if (arg == "--audio-rate-scale" || arg == "--probe-audio-rate-scale") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			try {
				request.audio_rate_scale = std::stod(*value);
			}
			catch (...) {
				error = arg + " requires a positive number\n" + Usage();
				return std::nullopt;
			}
			if (request.audio_rate_scale <= 0.0) {
				error = arg + " requires a positive number\n" + Usage();
				return std::nullopt;
			}
			continue;
		}
		if (arg == "--audio-quantum-ms" || arg == "--probe-audio-quantum-ms") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.audio_quantum_ms = *parsed;
			continue;
		}
		error = "unrecognized inspect media argument: " + arg + "\n" + Usage();
		return std::nullopt;
	}

	if (request.video_path.empty() && request.audio_path.empty()) {
		error = "inspect media requires --video or --audio\n" + Usage();
		return std::nullopt;
	}
	if (!request.skip_audio && request.audio_path.empty())
		request.audio_path = request.video_path;
	return request;
}

std::optional<AssInfoInspectRequest> ParseInspectAssInfoRequest(std::vector<std::string> const& args, std::string& error) {
	AssInfoInspectRequest request;

	if (args.size() == 5 && !args[4].empty() && args[4][0] != '-') {
		request.subtitle_path = agi::fs::path(args[4]);
		return request;
	}

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--input" || arg == "--subtitle" || arg == "--file") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.subtitle_path = agi::fs::path(*value);
			continue;
		}
		if (arg == "--encoding") {
			auto value = RequireValue(args, i, "--encoding", error);
			if (!value)
				return std::nullopt;
			request.encoding = *value;
			continue;
		}
		error = "unrecognized inspect ass-info argument: " + arg + "\n" + Usage();
		return std::nullopt;
	}

	if (request.subtitle_path.empty()) {
		error = "inspect ass-info requires a subtitle path\n" + Usage();
		return std::nullopt;
	}
	return request;
}

std::optional<BatchTraceSummarizeRequest> ParseBatchTraceSummarizeRequest(std::vector<std::string> const& args, std::string& error) {
	BatchTraceSummarizeRequest request;
	agi::fs::path list_file;

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--output-dir") {
			auto value = RequireValue(args, i, "--output-dir", error);
			if (!value)
				return std::nullopt;
			request.output_dir = agi::fs::path(*value);
			continue;
		}
		if (arg == "--list-file") {
			auto value = RequireValue(args, i, "--list-file", error);
			if (!value)
				return std::nullopt;
			list_file = agi::fs::path(*value);
			continue;
		}
		if (arg == "--input") {
			auto value = RequireValue(args, i, "--input", error);
			if (!value)
				return std::nullopt;
			request.inputs.emplace_back(*value);
			continue;
		}
		request.inputs.emplace_back(arg);
	}

	if (request.output_dir.empty()) {
		error = "--cli batch trace-summarize requires --output-dir\n" + Usage();
		return std::nullopt;
	}
	if (!list_file.empty() && !agi::fs::FileExists(list_file)) {
		error = "trace-summarize list file does not exist: " + ToGenericString(list_file);
		return std::nullopt;
	}
	request.inputs = ResolveBatchInputs(std::move(request.inputs), list_file);
	if (request.inputs.empty()) {
		error = "--cli batch trace-summarize requires at least one input path\n" + Usage();
		return std::nullopt;
	}
	return request;
}

std::optional<BatchAssInfoRequest> ParseBatchAssInfoRequest(std::vector<std::string> const& args, std::string& error) {
	BatchAssInfoRequest request;
	agi::fs::path list_file;

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--output-dir") {
			auto value = RequireValue(args, i, "--output-dir", error);
			if (!value)
				return std::nullopt;
			request.output_dir = agi::fs::path(*value);
			continue;
		}
		if (arg == "--list-file") {
			auto value = RequireValue(args, i, "--list-file", error);
			if (!value)
				return std::nullopt;
			list_file = agi::fs::path(*value);
			continue;
		}
		if (arg == "--input") {
			auto value = RequireValue(args, i, "--input", error);
			if (!value)
				return std::nullopt;
			request.inputs.emplace_back(*value);
			continue;
		}
		if (arg == "--encoding") {
			auto value = RequireValue(args, i, "--encoding", error);
			if (!value)
				return std::nullopt;
			request.encoding = *value;
			continue;
		}
		request.inputs.emplace_back(arg);
	}

	if (request.output_dir.empty()) {
		error = "--cli batch ass-info requires --output-dir\n" + Usage();
		return std::nullopt;
	}
	if (!list_file.empty() && !agi::fs::FileExists(list_file)) {
		error = "ass-info list file does not exist: " + ToGenericString(list_file);
		return std::nullopt;
	}
	request.inputs = ResolveBatchInputs(std::move(request.inputs), list_file);
	if (request.inputs.empty()) {
		error = "--cli batch ass-info requires at least one input path\n" + Usage();
		return std::nullopt;
	}
	return request;
}

}

ParseResult ParseCommandLine(std::vector<std::string> const& args) {
	ParseResult result;
	if (args.size() < 2 || args[1] != "--cli")
		return result;

	result.requested = true;
	if (args.size() < 4) {
		result.error = Usage();
		return result;
	}

	auto const& command = args[2];
	auto const& subcommand = args[3];
	if (command == "probe" && subcommand == "playback") {
		std::vector<std::string> probe_args;
		probe_args.reserve(args.size() - 3);
		probe_args.emplace_back(args.front());
		probe_args.insert(probe_args.end(), args.begin() + 4, args.end());

		auto parsed = headless_playback_probe::ParseRequestArguments(probe_args, true);
		if (!parsed.request) {
			result.error = parsed.error.empty() ? Usage() : parsed.error;
			return result;
		}
		result.command.emplace(ProbePlaybackCommand{std::move(*parsed.request)});
		return result;
	}

	if (command == "session" && subcommand == "playback") {
		auto request = ParseSessionPlaybackRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(SessionPlaybackCommand{std::move(*request)});
		return result;
	}

	if (command == "session" && subcommand == "project") {
		auto request = ParseSessionProjectRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(SessionProjectCommand{std::move(*request)});
		return result;
	}

	if (command == "inspect" && subcommand == "media") {
		auto request = ParseInspectMediaRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(InspectMediaCommand{std::move(*request)});
		return result;
	}

	if (command == "inspect" && subcommand == "ass-info") {
		auto request = ParseInspectAssInfoRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(InspectAssInfoCommand{std::move(*request)});
		return result;
	}

	if (command == "inspect" && subcommand == "trace") {
		TraceInspectRequest request;
		if (args.size() == 5) {
			request.input_path = agi::fs::path(args[4]);
		}
		else {
			for (size_t i = 4; i < args.size(); ++i) {
				if (args[i] == "--session-dir" || args[i] == "--trace-dir" || args[i] == "--input") {
					auto value = RequireValue(args, i, args[i], result.error);
					if (!value)
						return result;
					request.input_path = agi::fs::path(*value);
					continue;
				}
				result.error = "unrecognized inspect trace argument: " + args[i] + "\n" + Usage();
				return result;
			}
		}

		if (request.input_path.empty()) {
			result.error = "inspect trace requires a session directory or trace file path\n" + Usage();
			return result;
		}
		result.command.emplace(InspectTraceCommand{std::move(request)});
		return result;
	}

	if (command == "batch" && subcommand == "playback-probe") {
		BatchPlaybackProbeRequest request;
		std::vector<std::string> probe_args;
		probe_args.emplace_back(args.front());

		for (size_t i = 4; i < args.size(); ++i) {
			if (args[i] == "--list-file") {
				auto value = RequireValue(args, i, "--list-file", result.error);
				if (!value)
					return result;
				request.list_file = agi::fs::path(*value);
				continue;
			}
			if (args[i] == "--output-dir") {
				auto value = RequireValue(args, i, "--output-dir", result.error);
				if (!value)
					return result;
				request.output_dir = agi::fs::path(*value);
				continue;
			}
			probe_args.emplace_back(args[i]);
		}

		if (request.list_file.empty()) {
			result.error = "--cli batch playback-probe requires --list-file\n" + Usage();
			return result;
		}
		if (request.output_dir.empty()) {
			result.error = "--cli batch playback-probe requires --output-dir\n" + Usage();
			return result;
		}
		if (!agi::fs::FileExists(request.list_file)) {
			result.error = "batch playback probe list file does not exist: " + ToGenericString(request.list_file);
			return result;
		}

		for (size_t i = 1; i < probe_args.size(); ++i) {
			auto const& arg = probe_args[i];
			if (arg == "--probe-video" || arg == "--probe-audio" || arg == "--probe-trace-dir" || arg == "--headless-playback-probe") {
				result.error = "batch playback-probe does not accept per-item flag in common args: " + arg + "\n" + Usage();
				return result;
			}
		}

		auto parsed = headless_playback_probe::ParseRequestArguments(probe_args, false);
		if (!parsed.request) {
			result.error = parsed.error.empty() ? Usage() : parsed.error;
			return result;
		}
		request.probe_template = std::move(*parsed.request);
		result.command.emplace(BatchPlaybackProbeCommand{std::move(request)});
		return result;
	}

	if (command == "batch" && subcommand == "trace-summarize") {
		auto request = ParseBatchTraceSummarizeRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(BatchTraceSummarizeCommand{std::move(*request)});
		return result;
	}

	if (command == "batch" && subcommand == "ass-info") {
		auto request = ParseBatchAssInfoRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(BatchAssInfoCommand{std::move(*request)});
		return result;
	}

	result.error = "unrecognized CLI command: " + command + " " + subcommand + "\n" + Usage();
	return result;
}

std::string Usage() {
	return std::string(
		"Usage:\n"
		"  Aegisub.exe --cli probe playback [probe flags...]\n"
		"  Aegisub.exe --cli session playback --script-file <path> --video <path> [session flags...]\n"
		"  Aegisub.exe --cli session project --script-file <path> [project flags...]\n"
		"  Aegisub.exe --cli inspect media --video <path> [media flags...]\n"
		"  Aegisub.exe --cli inspect ass-info <path> [--encoding <name>]\n"
		"  Aegisub.exe --cli inspect trace <session-dir|manifest.txt|summary.txt|trace.ndjson>\n"
		"  Aegisub.exe --cli batch playback-probe --list-file <path> --output-dir <dir> [probe flags...]\n"
		"  Aegisub.exe --cli batch trace-summarize --output-dir <dir> [--list-file <path>|--input <path>...]\n"
		"  Aegisub.exe --cli batch ass-info --output-dir <dir> [--list-file <path>|--input <path>...] [--encoding <name>]\n"
		"\n"
		"Session script steps:\n"
		"  open | reopen | close | install-playline <start_ms> <duration_ms> | play | playline | stop\n"
		"  sleep <ms> | wait-playback-stop [timeout_ms] | jump-time <ms> | jump-frame <frame>\n"
		"  query-media | query-playback | assert-media <true|false> <true|false>\n"
		"  assert-playing <true|false> <true|false> | assert-authority <audio|video>\n"
		"\n"
		"Playback session flags:\n"
		"  --script-file <path> --video <path> [--audio <path>] [--skip-audio]\n"
		"  [--video-provider <name>] [--audio-provider <name>] [--trace-dir <path>]\n"
		"  [--video-track-index <index>] [--audio-track-index <index>]\n"
		"  [--audio-rate-scale <scale>] [--audio-quantum-ms <ms>]\n"
		"\n"
		"Project session steps:\n"
		"  open-media | reopen-media | close-media | open-subtitles | open-subtitles-unlinked\n"
		"  open-subtitles-from-video | close-subtitles | open-timecodes | close-timecodes\n"
		"  open-keyframes | close-keyframes | insert-dialogue <row> <start_ms> <end_ms> <comment> [text...]\n"
		"  delete-dialogue <row> | set-dialogue-times <row> <start_ms> <end_ms>\n"
		"  assert-dialogue <row> <start_ms> <end_ms> <comment> [text...] | save-subtitles | query-project\n"
		"  assert-project <true|false> <true|false> <true|false> <true|false> <true|false>\n"
		"  assert-subtitle-counts <style_count> <event_count> <dialogue_count> <comment_count>\n"
		"  assert-subtitle-modified <true|false>\n"
		"\n"
		"Project session flags:\n"
		"  --script-file <path> [--video <path>] [--audio <path>] [--skip-audio]\n"
		"  [--subtitle <path>] [--output-subtitle <path>] [--subtitle-encoding <name>] [--timecodes <path>] [--keyframes <path>]\n"
		"  [--video-provider <name>] [--audio-provider <name>] [--trace-dir <path>]\n"
		"  [--video-track-index <index>] [--audio-track-index <index>] [--subtitle-track-index <index>]\n"
		"  [--audio-rate-scale <scale>] [--audio-quantum-ms <ms>]\n"
		"\n"
		"Inspect media flags:\n"
		"  --video <path> [--audio <path>] [--skip-audio]\n"
		"  [--video-provider <name>] [--audio-provider <name>] [--trace-dir <path>]\n"
		"  [--video-track-index <index>] [--audio-track-index <index>] [--subtitle-track-index <index>]\n"
		"  [--audio-rate-scale <scale>] [--audio-quantum-ms <ms>]\n"
		"\n"
		"Batch list file syntax:\n"
		"  one path per line, or for playback-probe one video<TAB>audio per line\n"
		"\n"
		"Playback probe flags:\n"
		"  ") + headless_playback_probe::Usage();
}

}
