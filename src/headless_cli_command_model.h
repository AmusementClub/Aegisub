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

#pragma once

#include "ass_info_service.h"
#include "headless_playback_probe.h"
#include "media_inspect_service.h"
#include "project_session_service.h"
#include "playback_session_service.h"
#include "trace_summary_service.h"
#include "trace_inspect_service.h"

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace headless_cli {

using TraceInspectRequest = aegisub::trace_inspect_service::TraceInspectRequest;
using PlaybackSessionRequest = aegisub::playback_session_service::PlaybackSessionRequest;
using PlaybackSessionResult = aegisub::playback_session_service::PlaybackSessionResult;
using ProjectSessionRequest = aegisub::project_session_service::ProjectSessionRequest;
using ProjectSessionResult = aegisub::project_session_service::ProjectSessionResult;
using MediaInspectRequest = aegisub::media_inspect_service::MediaInspectRequest;
using MediaInspectResult = aegisub::media_inspect_service::MediaInspectResult;
using AssInfoInspectRequest = aegisub::ass_info_service::AssInfoInspectRequest;
using AssInfoInspectResult = aegisub::ass_info_service::AssInfoInspectResult;

struct TraceInspectResult {
	int exit_code = 0;
	std::string output;
	std::string error;
};

struct BatchPlaybackProbeRequest {
	agi::fs::path list_file;
	agi::fs::path output_dir;
	headless_playback_probe::PlaybackProbeRequest probe_template;
};

struct BatchPlaybackProbeResult {
	int exit_code = 0;
	size_t total_cases = 0;
	size_t passed_cases = 0;
	size_t failed_cases = 0;
	agi::fs::path output_dir;
	std::string message;
};

struct ProbePlaybackCommand {
	headless_playback_probe::PlaybackProbeRequest request;
};

struct SessionPlaybackCommand {
	PlaybackSessionRequest request;
};

struct SessionProjectCommand {
	ProjectSessionRequest request;
};

struct InspectMediaCommand {
	MediaInspectRequest request;
};

struct InspectAssInfoCommand {
	AssInfoInspectRequest request;
};

struct InspectTraceCommand {
	TraceInspectRequest request;
};

struct BatchPlaybackProbeCommand {
	BatchPlaybackProbeRequest request;
};

struct BatchTraceSummarizeRequest {
	std::vector<agi::fs::path> inputs;
	agi::fs::path output_dir;
};

struct BatchTraceSummarizeResult {
	int exit_code = 0;
	size_t total_sessions = 0;
	size_t passed_sessions = 0;
	size_t failed_sessions = 0;
	agi::fs::path output_dir;
	std::string message;
};

struct BatchTraceSummarizeCommand {
	BatchTraceSummarizeRequest request;
};

struct BatchAssInfoRequest {
	std::vector<agi::fs::path> inputs;
	agi::fs::path output_dir;
	std::string encoding;
};

struct BatchAssInfoResult {
	int exit_code = 0;
	size_t total_files = 0;
	size_t passed_files = 0;
	size_t failed_files = 0;
	agi::fs::path output_dir;
	std::string message;
};

struct BatchAssInfoCommand {
	BatchAssInfoRequest request;
};

using Command = std::variant<
	ProbePlaybackCommand,
	SessionPlaybackCommand,
	SessionProjectCommand,
	InspectMediaCommand,
	InspectAssInfoCommand,
	InspectTraceCommand,
	BatchPlaybackProbeCommand,
	BatchTraceSummarizeCommand,
	BatchAssInfoCommand>;

struct ParseResult {
	bool requested = false;
	std::optional<Command> command;
	std::string error;
};

}
