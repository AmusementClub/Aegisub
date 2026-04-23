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
#include "automation/automation_breakpoint_store.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

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

agi::fs::path PathFromUtf8Arg(std::string const& value) {
	return agi::fs::PathFromString(value);
}

std::optional<std::vector<int>> ParseIntegerListValue(std::string const& value) {
	std::vector<int> rows;
	std::stringstream stream(value);
	std::string item;
	while (std::getline(stream, item, ',')) {
		item = Trim(item);
		if (item.empty())
			continue;
		auto parsed = ParseIntegerValue(item);
		if (!parsed || *parsed <= 0)
			return std::nullopt;
		rows.push_back(*parsed);
	}
	return rows;
}

enum class ParseOptionResult {
	Unhandled,
	Parsed,
	Error
};

bool MatchesOption(std::string const& arg, std::string_view primary, std::string_view probe_alias = {})
{
	return arg == primary || (!probe_alias.empty() && arg == probe_alias);
}

ParseOptionResult ParsePathOption(
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string_view primary,
	std::string_view probe_alias,
	std::string& error,
	agi::fs::path& target)
{
	if (!MatchesOption(arg, primary, probe_alias))
		return ParseOptionResult::Unhandled;

	auto value = RequireValue(args, i, arg, error);
	if (!value)
		return ParseOptionResult::Error;

	target = PathFromUtf8Arg(*value);
	return ParseOptionResult::Parsed;
}

ParseOptionResult ParseStringOption(
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string_view primary,
	std::string_view probe_alias,
	std::string& error,
	std::optional<std::string>& target)
{
	if (!MatchesOption(arg, primary, probe_alias))
		return ParseOptionResult::Unhandled;

	auto value = RequireValue(args, i, arg, error);
	if (!value)
		return ParseOptionResult::Error;

	target = *value;
	return ParseOptionResult::Parsed;
}

ParseOptionResult ParseTrackIndexOption(
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string_view primary,
	std::string_view probe_alias,
	std::string& error,
	std::optional<int>& target)
{
	if (!MatchesOption(arg, primary, probe_alias))
		return ParseOptionResult::Unhandled;

	auto value = RequireValue(args, i, arg, error);
	if (!value)
		return ParseOptionResult::Error;

	auto parsed = ParseIntegerValue(*value);
	if (!parsed || *parsed < 0) {
		error = arg + " requires a non-negative integer\n" + Usage();
		return ParseOptionResult::Error;
	}

	target = *parsed;
	return ParseOptionResult::Parsed;
}

ParseOptionResult ParseTraceDirOption(
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string_view primary,
	std::string_view probe_alias,
	std::string& error,
	std::optional<agi::fs::path>& target)
{
	if (!MatchesOption(arg, primary, probe_alias))
		return ParseOptionResult::Unhandled;

	auto value = RequireValue(args, i, arg, error);
	if (!value)
		return ParseOptionResult::Error;

	target = PathFromUtf8Arg(*value);
	return ParseOptionResult::Parsed;
}

ParseOptionResult ParsePositiveDoubleOption(
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string_view primary,
	std::string_view probe_alias,
	std::string& error,
	double& target)
{
	if (!MatchesOption(arg, primary, probe_alias))
		return ParseOptionResult::Unhandled;

	auto value = RequireValue(args, i, arg, error);
	if (!value)
		return ParseOptionResult::Error;

	try {
		target = std::stod(*value);
	}
	catch (...) {
		error = arg + " requires a positive number\n" + Usage();
		return ParseOptionResult::Error;
	}

	if (target <= 0.0) {
		error = arg + " requires a positive number\n" + Usage();
		return ParseOptionResult::Error;
	}

	return ParseOptionResult::Parsed;
}

ParseOptionResult ParseNonNegativeIntegerOption(
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string_view primary,
	std::string_view probe_alias,
	std::string& error,
	int& target)
{
	if (!MatchesOption(arg, primary, probe_alias))
		return ParseOptionResult::Unhandled;

	auto value = RequireValue(args, i, arg, error);
	if (!value)
		return ParseOptionResult::Error;

	auto parsed = ParseIntegerValue(*value);
	if (!parsed || *parsed < 0) {
		error = arg + " requires a non-negative integer\n" + Usage();
		return ParseOptionResult::Error;
	}

	target = *parsed;
	return ParseOptionResult::Parsed;
}

struct CommonMediaRequestFields {
	agi::fs::path& video_path;
	agi::fs::path& audio_path;
	std::optional<std::string>& video_provider;
	std::optional<std::string>& audio_provider;
	std::optional<int>& video_track_index;
	std::optional<int>& audio_track_index;
	std::optional<int>* subtitle_track_index = nullptr;
	bool& skip_audio;
	double& audio_rate_scale;
	int& audio_quantum_ms;
	std::optional<agi::fs::path>& trace_dir;
};

ParseOptionResult TryParseCommonMediaArgument(
	CommonMediaRequestFields fields,
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string& error,
	bool allow_probe_aliases)
{
	auto const probe_video = allow_probe_aliases ? std::string_view{"--probe-video"} : std::string_view{};
	auto const probe_audio = allow_probe_aliases ? std::string_view{"--probe-audio"} : std::string_view{};
	auto const probe_skip_audio = allow_probe_aliases ? std::string_view{"--probe-skip-audio"} : std::string_view{};
	auto const probe_video_provider = allow_probe_aliases ? std::string_view{"--probe-video-provider"} : std::string_view{};
	auto const probe_audio_provider = allow_probe_aliases ? std::string_view{"--probe-audio-provider"} : std::string_view{};
	auto const probe_video_track = allow_probe_aliases ? std::string_view{"--probe-video-track-index"} : std::string_view{};
	auto const probe_audio_track = allow_probe_aliases ? std::string_view{"--probe-audio-track-index"} : std::string_view{};
	auto const probe_subtitle_track = allow_probe_aliases ? std::string_view{"--probe-subtitle-track-index"} : std::string_view{};
	auto const probe_trace_dir = allow_probe_aliases ? std::string_view{"--probe-trace-dir"} : std::string_view{};
	auto const probe_audio_rate = allow_probe_aliases ? std::string_view{"--probe-audio-rate-scale"} : std::string_view{};
	auto const probe_audio_quantum = allow_probe_aliases ? std::string_view{"--probe-audio-quantum-ms"} : std::string_view{};

	for (auto result : {
		ParsePathOption(args, i, arg, "--video", probe_video, error, fields.video_path),
		ParsePathOption(args, i, arg, "--audio", probe_audio, error, fields.audio_path),
		ParseStringOption(args, i, arg, "--video-provider", probe_video_provider, error, fields.video_provider),
		ParseStringOption(args, i, arg, "--audio-provider", probe_audio_provider, error, fields.audio_provider),
		ParseTrackIndexOption(args, i, arg, "--video-track-index", probe_video_track, error, fields.video_track_index),
		ParseTrackIndexOption(args, i, arg, "--audio-track-index", probe_audio_track, error, fields.audio_track_index),
		ParseTraceDirOption(args, i, arg, "--trace-dir", probe_trace_dir, error, fields.trace_dir),
		ParsePositiveDoubleOption(args, i, arg, "--audio-rate-scale", probe_audio_rate, error, fields.audio_rate_scale),
		ParseNonNegativeIntegerOption(args, i, arg, "--audio-quantum-ms", probe_audio_quantum, error, fields.audio_quantum_ms),
	}) {
		if (result != ParseOptionResult::Unhandled)
			return result;
	}

	if (fields.subtitle_track_index) {
		auto result = ParseTrackIndexOption(args, i, arg, "--subtitle-track-index", probe_subtitle_track, error, *fields.subtitle_track_index);
		if (result != ParseOptionResult::Unhandled)
			return result;
	}

	if (MatchesOption(arg, "--skip-audio", probe_skip_audio)) {
		fields.skip_audio = true;
		return ParseOptionResult::Parsed;
	}

	return ParseOptionResult::Unhandled;
}

struct CommonSubtitleIoRequestFields {
	agi::fs::path& subtitle_path;
	agi::fs::path& output_subtitle_path;
	agi::fs::path& timecodes_path;
	agi::fs::path& keyframes_path;
	std::string& subtitle_encoding;
	std::string* output_encoding = nullptr;
};

ParseOptionResult TryParseCommonSubtitleIoArgument(
	CommonSubtitleIoRequestFields fields,
	std::vector<std::string> const& args,
	size_t& i,
	std::string const& arg,
	std::string& error)
{
	for (auto result : {
		ParsePathOption(args, i, arg, "--subtitle", {}, error, fields.subtitle_path),
		ParsePathOption(args, i, arg, "--output-subtitle", {}, error, fields.output_subtitle_path),
		ParsePathOption(args, i, arg, "--timecodes", {}, error, fields.timecodes_path),
		ParsePathOption(args, i, arg, "--keyframes", {}, error, fields.keyframes_path),
	}) {
		if (result != ParseOptionResult::Unhandled)
			return result;
	}

	if (arg == "--subtitle-encoding") {
		auto value = RequireValue(args, i, arg, error);
		if (!value)
			return ParseOptionResult::Error;
		fields.subtitle_encoding = *value;
		return ParseOptionResult::Parsed;
	}

	if (fields.output_encoding && arg == "--output-encoding") {
		auto value = RequireValue(args, i, arg, error);
		if (!value)
			return ParseOptionResult::Error;
		*fields.output_encoding = *value;
		return ParseOptionResult::Parsed;
	}

	return ParseOptionResult::Unhandled;
}

std::optional<Automation4::AutomationDebugBreakpoint> ParseAutomationDebugBreakpointValue(
	std::string const& value,
	agi::fs::path const& default_source)
{
	auto trimmed = Trim(value);
	if (trimmed.empty())
		return std::nullopt;

	auto split = trimmed.rfind(':');
	std::string source_text;
	std::string line_text = trimmed;
	if (split != std::string::npos) {
		source_text = Trim(trimmed.substr(0, split));
		line_text = Trim(trimmed.substr(split + 1));
	}

	auto parsed_line = ParseIntegerValue(line_text);
	if (!parsed_line || *parsed_line <= 0)
		return std::nullopt;

	if (source_text.empty())
		source_text = ToGenericString(default_source);

	return Automation4::AutomationDebugBreakpoint{
		Automation4::NormalizeAutomationDebugSource(source_text),
		*parsed_line,
		true
	};
}

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
	auto in = agi::io::OpenInputFileStream(script_file, std::ios::in);
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
			script_file = PathFromUtf8Arg(*value);
			continue;
		}
		auto result = TryParseCommonMediaArgument({
			request.video_path,
			request.audio_path,
			request.video_provider,
			request.audio_provider,
			request.video_track_index,
			request.audio_track_index,
			nullptr,
			request.skip_audio,
			request.audio_rate_scale,
			request.audio_quantum_ms,
			request.trace_dir,
		}, args, i, arg, error, true);
		if (result == ParseOptionResult::Error)
			return std::nullopt;
		if (result == ParseOptionResult::Parsed)
			continue;

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
	auto in = agi::io::OpenInputFileStream(script_file, std::ios::in);
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
			script_file = PathFromUtf8Arg(*value);
			continue;
		}
		auto subtitle_result = TryParseCommonSubtitleIoArgument({
			request.subtitle_path,
			request.output_subtitle_path,
			request.timecodes_path,
			request.keyframes_path,
			request.subtitle_encoding,
			nullptr,
		}, args, i, arg, error);
		if (subtitle_result == ParseOptionResult::Error)
			return std::nullopt;
		if (subtitle_result == ParseOptionResult::Parsed)
			continue;

		auto media_result = TryParseCommonMediaArgument({
			request.video_path,
			request.audio_path,
			request.video_provider,
			request.audio_provider,
			request.video_track_index,
			request.audio_track_index,
			&request.subtitle_track_index,
			request.skip_audio,
			request.audio_rate_scale,
			request.audio_quantum_ms,
			request.trace_dir,
		}, args, i, arg, error, false);
		if (media_result == ParseOptionResult::Error)
			return std::nullopt;
		if (media_result == ParseOptionResult::Parsed)
			continue;

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

std::optional<AutomationSessionRequest> ParseSessionAutomationRequest(std::vector<std::string> const& args, std::string& error) {
	AutomationSessionRequest request;
	bool saw_macro = false;
	bool saw_filter = false;
	std::vector<std::string> raw_debug_breakpoints;

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--script" || arg == "--automation-script" || arg == "--script-file") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.script_path = PathFromUtf8Arg(*value);
			continue;
		}
		if (arg == "--macro") {
			auto value = RequireValue(args, i, "--macro", error);
			if (!value)
				return std::nullopt;
			request.feature_name = *value;
			request.feature_kind = aegisub::automation_session_service::AutomationSessionFeatureKind::Macro;
			saw_macro = true;
			continue;
		}
		if (arg == "--filter") {
			auto value = RequireValue(args, i, "--filter", error);
			if (!value)
				return std::nullopt;
			request.feature_name = *value;
			request.feature_kind = aegisub::automation_session_service::AutomationSessionFeatureKind::ExportFilter;
			saw_filter = true;
			continue;
		}
		auto subtitle_result = TryParseCommonSubtitleIoArgument({
			request.subtitle_path,
			request.output_subtitle_path,
			request.timecodes_path,
			request.keyframes_path,
			request.subtitle_encoding,
			&request.output_encoding,
		}, args, i, arg, error);
		if (subtitle_result == ParseOptionResult::Error)
			return std::nullopt;
		if (subtitle_result == ParseOptionResult::Parsed)
			continue;

		auto media_result = TryParseCommonMediaArgument({
			request.video_path,
			request.audio_path,
			request.video_provider,
			request.audio_provider,
			request.video_track_index,
			request.audio_track_index,
			&request.subtitle_track_index,
			request.skip_audio,
			request.audio_rate_scale,
			request.audio_quantum_ms,
			request.trace_dir,
		}, args, i, arg, error, false);
		if (media_result == ParseOptionResult::Error)
			return std::nullopt;
		if (media_result == ParseOptionResult::Parsed)
			continue;
		if (arg == "--selection" || arg == "--selected-rows") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto rows = ParseIntegerListValue(*value);
			if (!rows) {
				error = arg + " requires a comma-separated list of positive integers\n" + Usage();
				return std::nullopt;
			}
			request.selected_rows = std::move(*rows);
			continue;
		}
		if (arg == "--active-row") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed <= 0) {
				error = arg + " requires a positive integer\n" + Usage();
				return std::nullopt;
			}
			request.active_row = *parsed;
			continue;
		}
		if (arg == "--debug-stop-on-entry") {
			request.debug.enabled = true;
			request.debug.stop_on_entry = true;
			continue;
		}
		if (arg == "--debug-breakpoint") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.debug.enabled = true;
			raw_debug_breakpoints.push_back(*value);
			continue;
		}
		if (arg == "--debug-auto-step") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed < 0) {
				error = arg + " requires a non-negative integer\n" + Usage();
				return std::nullopt;
			}
			request.debug.enabled = true;
			request.debug.auto_step_count = *parsed;
			continue;
		}
		if (arg == "--debug-max-pauses") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			auto parsed = ParseIntegerValue(*value);
			if (!parsed || *parsed <= 0) {
				error = arg + " requires a positive integer\n" + Usage();
				return std::nullopt;
			}
			request.debug.enabled = true;
			request.debug.max_pauses = static_cast<size_t>(*parsed);
			continue;
		}

		error = "unrecognized session automation argument: " + arg + "\n" + Usage();
		return std::nullopt;
	}

	if (request.script_path.empty()) {
		error = "--cli session automation requires --script\n" + Usage();
		return std::nullopt;
	}
	if (!agi::fs::FileExists(request.script_path)) {
		error = "automation script file does not exist: " + ToGenericString(request.script_path);
		return std::nullopt;
	}
	if (saw_macro == saw_filter) {
		error = "--cli session automation requires exactly one of --macro or --filter\n" + Usage();
		return std::nullopt;
	}
	if (request.feature_name.empty()) {
		error = "automation session requires a feature name\n" + Usage();
		return std::nullopt;
	}
	for (auto const& breakpoint_value : raw_debug_breakpoints) {
		auto breakpoint = ParseAutomationDebugBreakpointValue(breakpoint_value, request.script_path);
		if (!breakpoint) {
			error = "--debug-breakpoint requires <source:line> or <line>\n" + Usage();
			return std::nullopt;
		}
		request.debug.breakpoints.push_back(std::move(*breakpoint));
	}
	if (!request.skip_audio && request.audio_path.empty() && !request.video_path.empty())
		request.audio_path = request.video_path;
	if (request.output_encoding.empty())
		request.output_encoding = request.subtitle_encoding;

	return request;
}

std::optional<MediaInspectRequest> ParseInspectMediaRequest(std::vector<std::string> const& args, std::string& error) {
	MediaInspectRequest request;

	if (args.size() == 5 && !args[4].empty() && args[4][0] != '-') {
		request.video_path = PathFromUtf8Arg(args[4]);
		request.audio_path = request.video_path;
		return request;
	}

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		auto result = TryParseCommonMediaArgument({
			request.video_path,
			request.audio_path,
			request.video_provider,
			request.audio_provider,
			request.video_track_index,
			request.audio_track_index,
			&request.subtitle_track_index,
			request.skip_audio,
			request.audio_rate_scale,
			request.audio_quantum_ms,
			request.trace_dir,
		}, args, i, arg, error, true);
		if (result == ParseOptionResult::Error)
			return std::nullopt;
		if (result == ParseOptionResult::Parsed)
			continue;
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
		request.subtitle_path = PathFromUtf8Arg(args[4]);
		return request;
	}

	for (size_t i = 4; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--input" || arg == "--subtitle" || arg == "--file") {
			auto value = RequireValue(args, i, arg, error);
			if (!value)
				return std::nullopt;
			request.subtitle_path = PathFromUtf8Arg(*value);
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

	if (command == "session" && subcommand == "automation") {
		auto request = ParseSessionAutomationRequest(args, result.error);
		if (!request) {
			if (result.error.empty())
				result.error = Usage();
			return result;
		}
		result.command.emplace(SessionAutomationCommand{std::move(*request)});
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
			request.input_path = PathFromUtf8Arg(args[4]);
		}
		else {
			for (size_t i = 4; i < args.size(); ++i) {
				if (args[i] == "--session-dir" || args[i] == "--trace-dir" || args[i] == "--input") {
					auto value = RequireValue(args, i, args[i], result.error);
					if (!value)
						return result;
					request.input_path = PathFromUtf8Arg(*value);
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

	result.error = "unrecognized CLI command: " + command + " " + subcommand + "\n" + Usage();
	return result;
}

std::string Usage() {
	return std::string(
		"Usage:\n"
		"  Aegisub.exe --cli probe playback [probe flags...]\n"
		"  Aegisub.exe --cli session playback --script-file <path> --video <path> [session flags...]\n"
		"  Aegisub.exe --cli session project --script-file <path> [project flags...]\n"
		"  Aegisub.exe --cli session automation --script <path> (--macro <name>|--filter <name>) [automation flags...]\n"
		"  Aegisub.exe --cli inspect media --video <path> [media flags...]\n"
		"  Aegisub.exe --cli inspect ass-info <path> [--encoding <name>]\n"
		"  Aegisub.exe --cli inspect trace <session-dir|manifest.txt|summary.txt|trace.ndjson>\n"
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
		"Automation session flags:\n"
		"  --script <path> (--macro <name>|--filter <name>) [--subtitle <path>] [--output-subtitle <path>]\n"
		"  [--subtitle-encoding <name>] [--output-encoding <name>] [--video <path>] [--audio <path>] [--skip-audio]\n"
		"  [--timecodes <path>] [--keyframes <path>] [--selection <row,row,...>] [--active-row <row>]\n"
		"  [--debug-stop-on-entry] [--debug-breakpoint <source:line>|<line>] [--debug-auto-step <count>] [--debug-max-pauses <count>]\n"
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
		"Playback probe flags:\n"
		"  ") + headless_playback_probe::Usage();
}

}
