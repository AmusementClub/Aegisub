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

#include "headless_cli.h"
#include "playback_probe_service.h"
#include "trace_inspect_service.h"

#include <libaegisub/fs.h>
#include <libaegisub/exception.h>

#include <wx/app.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace headless_cli {
namespace {

struct BatchCaseSpec {
	agi::fs::path video_path;
	std::optional<agi::fs::path> audio_path;
};

struct BatchCaseResult {
	size_t index = 0;
	BatchCaseSpec spec;
	headless_playback_probe::PlaybackProbeResult probe_result;
};

std::string JsonEscape(std::string const& input) {
	std::string escaped;
	escaped.reserve(input.size());
	for (unsigned char c : input) {
		switch (c) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if (c < 0x20) {
				char buffer[7];
				snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(c));
				escaped += buffer;
			}
			else {
				escaped += static_cast<char>(c);
			}
			break;
		}
	}
	return escaped;
}

std::string CsvEscape(std::string const& input) {
	if (input.find_first_of(",\"\r\n") == std::string::npos)
		return input;

	std::string escaped = "\"";
	for (char c : input) {
		if (c == '"')
			escaped += "\"\"";
		else
			escaped += c;
	}
	escaped += "\"";
	return escaped;
}

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

bool TryParseInteger(std::string const& value) {
	if (value.empty())
		return false;
	size_t index = (value[0] == '-' || value[0] == '+') ? 1 : 0;
	if (index == value.size())
		return false;
	for (; index < value.size(); ++index) {
		if (!std::isdigit(static_cast<unsigned char>(value[index])))
			return false;
	}
	return true;
}

bool TryParseFloat(std::string const& value) {
	if (value.empty() || value.find_first_of(".eE") == std::string::npos)
		return false;
	char *end = nullptr;
	std::strtod(value.c_str(), &end);
	return end && *end == '\0';
}

void AppendJsonTypedValue(std::ostringstream& out, std::string const& value) {
	if (value == "true" || value == "false") {
		out << value;
		return;
	}
	if (TryParseInteger(value) || TryParseFloat(value)) {
		out << value;
		return;
	}
	out << '"' << JsonEscape(value) << '"';
}

std::string KeyValueMapToJson(std::map<std::string, std::string> const& values, int indent) {
	std::ostringstream out;
	std::string padding(indent, ' ');
	std::string next_padding(indent + 2, ' ');
	out << "{\n";
	bool first = true;
	for (auto const& [key, value] : values) {
		if (!first)
			out << ",\n";
		first = false;
		out << next_padding << '"' << JsonEscape(key) << "\": ";
		AppendJsonTypedValue(out, value);
	}
	if (!values.empty())
		out << '\n';
	out << padding << '}';
	return out.str();
}

std::optional<std::string> RequireValue(std::vector<std::string> const& args, size_t& index, std::string const& flag, std::string& error) {
	if (index + 1 >= args.size()) {
		error = flag + " requires a value\n" + Usage();
		return std::nullopt;
	}
	++index;
	return args[index];
}

std::string BuildTraceInspectJson(aegisub::trace_inspect_service::TraceSessionSummary const& session) {
	std::ostringstream out;
	out << "{\n";
	out << "  \"session_dir\": \"" << JsonEscape(ToGenericString(session.session_dir)) << "\",\n";
	out << "  \"manifest\": " << KeyValueMapToJson(session.manifest, 2) << ",\n";
	out << "  \"summary\": " << KeyValueMapToJson(session.summary, 2) << "\n";
	out << "}\n";
	return out.str();
}

std::string CaseDirectoryName(size_t index) {
	std::ostringstream out;
	out << "case-" << std::setfill('0') << std::setw(4) << (index + 1);
	return out.str();
}

std::vector<BatchCaseSpec> ReadBatchCaseList(agi::fs::path const& list_file) {
	std::ifstream in(list_file, std::ios::in);
	std::vector<BatchCaseSpec> cases;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;

		BatchCaseSpec spec;
		auto split = line.find('\t');
		if (split == std::string::npos) {
			spec.video_path = agi::fs::path(line);
		}
		else {
			spec.video_path = agi::fs::path(line.substr(0, split));
			auto audio_text = line.substr(split + 1);
			if (!audio_text.empty())
				spec.audio_path = agi::fs::path(audio_text);
		}
		if (!spec.video_path.empty())
			cases.emplace_back(std::move(spec));
	}
	return cases;
}

std::string Trim(std::string value) {
	auto const not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::vector<std::string> SplitWhitespace(std::string const& text) {
	std::istringstream in(text);
	std::vector<std::string> tokens;
	std::string token;
	while (in >> token)
		tokens.push_back(token);
	return tokens;
}

std::optional<int> ParseIntegerValue(std::string const& text) {
	if (!TryParseInteger(text))
		return std::nullopt;
	try {
		return std::stoi(text);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<bool> ParseBoolValue(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (value == "true")
		return true;
	if (value == "false")
		return false;
	return std::nullopt;
}

std::optional<aegisub::playback_session_service::PlaybackAuthorityKind> ParseAuthorityValue(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (value == "audio")
		return aegisub::playback_session_service::PlaybackAuthorityKind::Audio;
	if (value == "video")
		return aegisub::playback_session_service::PlaybackAuthorityKind::Video;
	return std::nullopt;
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

void WriteBatchResultsCsv(agi::fs::path const& output_dir, std::vector<BatchCaseResult> const& results) {
	std::ofstream out(output_dir / "results.csv", std::ios::out | std::ios::trunc);
	out << "index,passed,exit_code,video_path,audio_path,performed_seeks,seek_samples,audio_timer_samples,seek_max_abs_delta_ms,seek_mean_abs_delta_ms,actual_video_decoder,actual_audio_provider,trace_dir,message\n";
	for (auto const& result : results) {
		out
			<< result.index + 1 << ','
			<< (result.probe_result.passed ? "true" : "false") << ','
			<< result.probe_result.exit_code << ','
			<< CsvEscape(ToGenericString(result.spec.video_path)) << ','
			<< CsvEscape(ToGenericString(result.spec.audio_path.value_or(result.spec.video_path))) << ','
			<< result.probe_result.performed_seeks << ','
			<< result.probe_result.seek_samples << ','
			<< result.probe_result.audio_timer_samples << ','
			<< result.probe_result.max_abs_delta_ms << ','
			<< result.probe_result.mean_abs_delta_ms << ','
			<< CsvEscape(result.probe_result.actual_video_decoder) << ','
			<< CsvEscape(result.probe_result.actual_audio_provider) << ','
			<< CsvEscape(ToGenericString(result.probe_result.trace_dir)) << ','
			<< CsvEscape(result.probe_result.message)
			<< "\n";
	}
}

void WriteBatchSummary(agi::fs::path const& output_dir, agi::fs::path const& list_file, BatchPlaybackProbeResult const& result) {
	std::ofstream out(output_dir / "summary.txt", std::ios::out | std::ios::trunc);
	out << "command=batch playback-probe\n";
	out << "list_file=" << ToGenericString(list_file) << "\n";
	out << "output_dir=" << ToGenericString(result.output_dir) << "\n";
	out << "total_cases=" << result.total_cases << "\n";
	out << "passed_cases=" << result.passed_cases << "\n";
	out << "failed_cases=" << result.failed_cases << "\n";
	out << "result=" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\n";
	out << "message=" << result.message << "\n";
}

void WriteBatchManifestJson(agi::fs::path const& output_dir, agi::fs::path const& list_file, std::vector<BatchCaseResult> const& results, BatchPlaybackProbeResult const& result) {
	std::ofstream out(output_dir / "manifest.json", std::ios::out | std::ios::trunc);
	out << "{\n";
	out << "  \"command\": \"batch playback-probe\",\n";
	out << "  \"list_file\": \"" << JsonEscape(ToGenericString(list_file)) << "\",\n";
	out << "  \"output_dir\": \"" << JsonEscape(ToGenericString(result.output_dir)) << "\",\n";
	out << "  \"total_cases\": " << result.total_cases << ",\n";
	out << "  \"passed_cases\": " << result.passed_cases << ",\n";
	out << "  \"failed_cases\": " << result.failed_cases << ",\n";
	out << "  \"result\": \"" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\",\n";
	out << "  \"message\": \"" << JsonEscape(result.message) << "\",\n";
	out << "  \"cases\": [\n";
	out << std::boolalpha;
	for (size_t i = 0; i < results.size(); ++i) {
		auto const& item = results[i];
		if (i)
			out << ",\n";
		out << "    {\n";
		out << "      \"index\": " << item.index + 1 << ",\n";
		out << "      \"video_path\": \"" << JsonEscape(ToGenericString(item.spec.video_path)) << "\",\n";
		out << "      \"audio_path\": \"" << JsonEscape(ToGenericString(item.spec.audio_path.value_or(item.spec.video_path))) << "\",\n";
		out << "      \"passed\": " << item.probe_result.passed << ",\n";
		out << "      \"exit_code\": " << item.probe_result.exit_code << ",\n";
		out << "      \"performed_seeks\": " << item.probe_result.performed_seeks << ",\n";
		out << "      \"seek_max_abs_delta_ms\": " << item.probe_result.max_abs_delta_ms << ",\n";
		out << "      \"seek_mean_abs_delta_ms\": " << item.probe_result.mean_abs_delta_ms << ",\n";
		out << "      \"actual_video_decoder\": \"" << JsonEscape(item.probe_result.actual_video_decoder) << "\",\n";
		out << "      \"actual_audio_provider\": \"" << JsonEscape(item.probe_result.actual_audio_provider) << "\",\n";
		out << "      \"trace_dir\": \"" << JsonEscape(ToGenericString(item.probe_result.trace_dir)) << "\",\n";
		out << "      \"message\": \"" << JsonEscape(item.probe_result.message) << "\"\n";
		out << "    }";
	}
	out << "\n  ]\n";
	out << "}\n";
}

class BatchPlaybackProbeRunner final {
	BatchPlaybackProbeRequest request;
	std::function<void(BatchPlaybackProbeResult)> on_done;
	std::vector<BatchCaseSpec> cases;
	std::vector<BatchCaseResult> results;
	size_t next_index = 0;
	bool finished = false;

	void RecordCaseFailure(size_t index, BatchCaseSpec spec, agi::fs::path trace_dir, std::string message) {
		headless_playback_probe::PlaybackProbeResult probe_result;
		probe_result.exit_code = 70;
		probe_result.trace_dir = std::move(trace_dir);
		probe_result.message = std::move(message);
		results.push_back(BatchCaseResult{
			index,
			std::move(spec),
			std::move(probe_result),
		});
		RunNext();
	}

	void Finish(BatchPlaybackProbeResult result) {
		if (finished)
			return;
		finished = true;
		result.output_dir = request.output_dir;
		if (result.message.empty()) {
			result.message = result.exit_code == 0
				? "batch playback probe completed"
				: "batch playback probe completed with failures";
		}
		bool const directory_ready = agi::fs::DirectoryExists(request.output_dir) || agi::fs::CreateDirectory(request.output_dir);
		if (directory_ready) {
			WriteBatchResultsCsv(request.output_dir, results);
			WriteBatchSummary(request.output_dir, request.list_file, result);
			WriteBatchManifestJson(request.output_dir, request.list_file, results, result);
		}
		if (on_done)
			on_done(std::move(result));
		delete this;
	}

	void RunNext() {
		try {
			if (next_index >= cases.size()) {
				BatchPlaybackProbeResult result;
				result.total_cases = results.size();
				for (auto const& item : results) {
					if (item.probe_result.passed)
						++result.passed_cases;
					else
						++result.failed_cases;
				}
				result.exit_code = result.failed_cases == 0 ? 0 : 1;
				Finish(std::move(result));
				return;
			}

			auto const index = next_index++;
			auto const spec = cases[index];
			auto const trace_dir = request.output_dir / CaseDirectoryName(index);

			auto probe_request = request.probe_template;
			probe_request.video_path = spec.video_path;
			if (probe_request.skip_audio) {
				probe_request.audio_path.clear();
			}
			else if (spec.audio_path) {
				probe_request.audio_path = *spec.audio_path;
			}
			else {
				probe_request.audio_path = spec.video_path;
			}
			probe_request.trace_dir = trace_dir;

			aegisub::playback_probe_service::RunAsync(std::move(probe_request), [this, index, spec](headless_playback_probe::PlaybackProbeResult probe_result) mutable {
				results.push_back(BatchCaseResult{
					index,
					std::move(spec),
					std::move(probe_result),
				});
				RunNext();
			});
		}
		catch (std::exception const& error) {
			auto const failed_index = next_index - 1;
			auto failed_spec = cases[failed_index];
			auto const trace_dir = request.output_dir / CaseDirectoryName(failed_index);
			RecordCaseFailure(failed_index, std::move(failed_spec), trace_dir, error.what());
		}
		catch (agi::Exception const& error) {
			auto const failed_index = next_index - 1;
			auto failed_spec = cases[failed_index];
			auto const trace_dir = request.output_dir / CaseDirectoryName(failed_index);
			RecordCaseFailure(failed_index, std::move(failed_spec), trace_dir, error.GetMessage());
		}
		catch (...) {
			auto const failed_index = next_index - 1;
			auto failed_spec = cases[failed_index];
			auto const trace_dir = request.output_dir / CaseDirectoryName(failed_index);
			RecordCaseFailure(failed_index, std::move(failed_spec), trace_dir, "unhandled exception");
		}
	}

public:
	BatchPlaybackProbeRunner(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done)) {
	}

	void Start() {
		try {
			agi::fs::CreateDirectory(request.output_dir);
			cases = ReadBatchCaseList(request.list_file);
			if (cases.empty()) {
				BatchPlaybackProbeResult result;
				result.exit_code = 2;
				result.message = "batch playback probe list is empty";
				Finish(std::move(result));
				return;
			}
			RunNext();
		}
		catch (std::exception const& error) {
			BatchPlaybackProbeResult result;
			result.exit_code = 2;
			result.message = error.what();
			Finish(std::move(result));
		}
	}
};

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

	result.error = "unrecognized CLI command: " + command + " " + subcommand + "\n" + Usage();
	return result;
}

TraceInspectResult RunInspectTrace(TraceInspectRequest const& request) {
	TraceInspectResult result;
	auto service_result = aegisub::trace_inspect_service::Inspect(request);
	if (!service_result.session) {
		result.exit_code = 2;
		result.error = service_result.error;
		return result;
	}
	result.output = BuildTraceInspectJson(*service_result.session);
	return result;
}

void RunSessionPlaybackAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done) {
	aegisub::playback_session_service::RunAsync(std::move(request), std::move(on_done));
}

void RunBatchPlaybackProbeAsync(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done) {
	auto *runner = new BatchPlaybackProbeRunner(std::move(request), std::move(on_done));
	runner->Start();
}

std::string Usage() {
	return std::string(
		"Usage:\n"
		"  Aegisub.exe --cli probe playback [probe flags...]\n"
		"  Aegisub.exe --cli session playback --script-file <path> --video <path> [session flags...]\n"
		"  Aegisub.exe --cli inspect trace <session-dir|manifest.txt|summary.txt|trace.ndjson>\n"
		"  Aegisub.exe --cli batch playback-probe --list-file <path> --output-dir <dir> [probe flags...]\n"
		"\n"
		"Session script steps:\n"
		"  open | reopen | close | install-playline <start_ms> <duration_ms> | play | playline | stop\n"
		"  sleep <ms> | wait-playback-stop [timeout_ms] | jump-time <ms> | jump-frame <frame>\n"
		"  query-media | query-playback | assert-media <true|false> <true|false>\n"
		"  assert-playing <true|false> <true|false> | assert-authority <audio|video>\n"
		"\n"
		"Session flags:\n"
		"  --script-file <path> --video <path> [--audio <path>] [--skip-audio]\n"
		"  [--video-provider <name>] [--audio-provider <name>] [--trace-dir <path>]\n"
		"  [--audio-rate-scale <scale>] [--audio-quantum-ms <ms>]\n"
		"\n"
		"Batch list file syntax:\n"
		"  one video path per line, or video<TAB>audio per line\n"
		"\n"
		"Playback probe flags:\n"
		"  ") + headless_playback_probe::Usage();
}

}
