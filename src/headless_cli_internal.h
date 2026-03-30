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

#include "headless_cli_command_model.h"

#include <optional>
#include <string>
#include <vector>

namespace headless_cli::detail {

struct BatchCaseSpec {
	agi::fs::path video_path;
	std::optional<agi::fs::path> audio_path;
};

struct BatchCaseResult {
	size_t index = 0;
	BatchCaseSpec spec;
	headless_playback_probe::PlaybackProbeResult probe_result;
};

std::string JsonEscape(std::string const& input);
std::string CsvEscape(std::string const& input);
std::string ToGenericString(agi::fs::path const& path);
std::optional<std::string> RequireValue(std::vector<std::string> const& args, size_t& index, std::string const& flag, std::string& error);
std::string BuildTraceInspectJson(aegisub::trace_inspect_service::TraceSessionSummary const& session);
std::string CaseDirectoryName(size_t index);
std::vector<BatchCaseSpec> ReadBatchCaseList(agi::fs::path const& list_file);
std::string Trim(std::string value);
std::vector<std::string> SplitWhitespace(std::string const& text);
std::optional<int> ParseIntegerValue(std::string const& text);
std::optional<bool> ParseBoolValue(std::string value);
std::optional<aegisub::playback_session_service::PlaybackAuthorityKind> ParseAuthorityValue(std::string value);

}
