#pragma once

#include <libaegisub/fs_fwd.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::trace_summary_service {

struct TraceSummaryRow {
	agi::fs::path input_path;
	agi::fs::path session_dir;
	std::string command;
	std::string build;
	std::string result;
	std::string video_path;
	std::string audio_path;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	std::string actual_video_provider;
	std::string actual_video_decoder;
	std::string actual_audio_provider_factory;
	std::string actual_audio_provider;
	bool video_provider_fallback = false;
	bool audio_provider_fallback = false;
	int probe_performed_seeks = 0;
	int probe_seek_max_abs_delta_ms = 0;
	double probe_seek_mean_abs_delta_ms = 0.0;
	int session_open_count = 0;
	int session_reopen_count = 0;
	int session_close_count = 0;
	int session_query_count = 0;
	int session_play_count = 0;
	int session_playline_count = 0;
	int session_stop_count = 0;
	int session_jump_time_count = 0;
	int session_jump_frame_count = 0;
	bool session_final_playback_uses_audio_authority = false;
	std::map<std::string, std::string> manifest;
	std::map<std::string, std::string> summary;
};

struct TraceSummaryResult {
	std::vector<TraceSummaryRow> rows;
	std::string error;
};

TraceSummaryResult Summarize(std::vector<agi::fs::path> const& inputs);

}
