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

#include "trace_summary_service.h"

#include "trace_inspect_service.h"

#include <algorithm>

namespace aegisub::trace_summary_service {
namespace {

std::string GetFirstNonEmpty(std::map<std::string, std::string> const& values, std::initializer_list<char const*> keys) {
	for (auto const* key : keys) {
		auto it = values.find(key);
		if (it != values.end() && !it->second.empty())
			return it->second;
	}
	return {};
}

bool ParseBool(std::map<std::string, std::string> const& values, char const* key) {
	auto it = values.find(key);
	return it != values.end() && it->second == "true";
}

int ParseInt(std::map<std::string, std::string> const& values, char const* key) {
	auto it = values.find(key);
	if (it == values.end() || it->second.empty())
		return 0;
	try {
		return std::stoi(it->second);
	}
	catch (...) {
		return 0;
	}
}

double ParseDouble(std::map<std::string, std::string> const& values, char const* key) {
	auto it = values.find(key);
	if (it == values.end() || it->second.empty())
		return 0.0;
	try {
		return std::stod(it->second);
	}
	catch (...) {
		return 0.0;
	}
}

TraceSummaryRow BuildRow(agi::fs::path const& input_path, trace_inspect_service::TraceSessionSummary const& session) {
	TraceSummaryRow row;
	row.input_path = input_path;
	row.session_dir = session.session_dir;
	row.command = GetFirstNonEmpty(session.manifest, { "command" });
	row.build = GetFirstNonEmpty(session.manifest, { "build" });
	row.result = GetFirstNonEmpty(session.summary, { "session.result", "probe.result", "result" });
	row.video_path = GetFirstNonEmpty(session.manifest, { "session.video", "video" });
	row.audio_path = GetFirstNonEmpty(session.manifest, { "session.audio", "audio" });
	row.selected_video_provider = GetFirstNonEmpty(session.summary, { "session.selected.video_provider", "probe.selected.video_provider", "selected.video_provider" });
	row.selected_audio_provider = GetFirstNonEmpty(session.summary, { "session.selected.audio_provider", "probe.selected.audio_provider", "selected.audio_provider" });
	row.actual_video_provider = GetFirstNonEmpty(session.summary, { "session.actual.video_provider", "probe.actual.video_provider", "actual.video_provider" });
	row.actual_video_decoder = GetFirstNonEmpty(session.summary, { "session.actual.video_decoder", "probe.actual.video_decoder", "actual.video_decoder", "media.video_decoder_name" });
	row.actual_audio_provider_factory = GetFirstNonEmpty(session.summary, { "session.actual.audio_provider_factory", "probe.actual.audio_provider_factory", "actual.audio_provider_factory" });
	row.actual_audio_provider = GetFirstNonEmpty(session.summary, { "session.actual.audio_provider", "probe.actual.audio_provider", "actual.audio_provider", "media.audio_provider_name" });
	row.video_provider_fallback = ParseBool(session.summary, "session.video.provider_fallback")
		|| ParseBool(session.summary, "probe.video.provider_fallback")
		|| ParseBool(session.summary, "video.provider_fallback");
	row.audio_provider_fallback = ParseBool(session.summary, "session.audio.provider_fallback")
		|| ParseBool(session.summary, "probe.audio.provider_fallback")
		|| ParseBool(session.summary, "audio.provider_fallback");
	row.probe_performed_seeks = ParseInt(session.summary, "probe.performed_seeks");
	row.probe_seek_max_abs_delta_ms = ParseInt(session.summary, "probe.seek.max_abs_delta_ms");
	row.probe_seek_mean_abs_delta_ms = ParseDouble(session.summary, "probe.seek.mean_abs_delta_ms");
	row.session_open_count = ParseInt(session.summary, "session.open_count");
	row.session_reopen_count = ParseInt(session.summary, "session.reopen_count");
	row.session_close_count = ParseInt(session.summary, "session.close_count");
	row.session_query_count = ParseInt(session.summary, "session.query_count");
	row.session_play_count = ParseInt(session.summary, "session.play_count");
	row.session_playline_count = ParseInt(session.summary, "session.playline_count");
	row.session_stop_count = ParseInt(session.summary, "session.stop_count");
	row.session_jump_time_count = ParseInt(session.summary, "session.jump_time_count");
	row.session_jump_frame_count = ParseInt(session.summary, "session.jump_frame_count");
	row.session_final_playback_uses_audio_authority = ParseBool(session.summary, "session.final.playback_uses_audio_authority");
	row.manifest = session.manifest;
	row.summary = session.summary;
	return row;
}

}

TraceSummaryResult Summarize(std::vector<agi::fs::path> const& inputs) {
	TraceSummaryResult result;
	result.rows.reserve(inputs.size());

	for (auto const& input : inputs) {
		auto inspect = trace_inspect_service::Inspect({input});
		if (!inspect.session) {
			result.error = inspect.error;
			result.rows.clear();
			return result;
		}
		result.rows.emplace_back(BuildRow(input, *inspect.session));
	}

	std::sort(result.rows.begin(), result.rows.end(), [](TraceSummaryRow const& left, TraceSummaryRow const& right) {
		return left.session_dir < right.session_dir;
	});
	return result;
}

}
