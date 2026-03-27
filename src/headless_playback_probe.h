#pragma once

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace headless_playback_probe {

struct PlaybackProbeRequest {
	agi::fs::path video_path;
	agi::fs::path audio_path;
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	bool skip_audio = false;
	int line_start_ms = 0;
	int repeat_count = 1;
	int repeat_gap_ms = 0;
	std::optional<int> seek_after_ms;
	std::optional<int> seek_target_offset_ms;
	int duration_ms = 2000;
	double audio_rate_scale = 0.9;
	int audio_quantum_ms = 0;
	int max_allowed_abs_delta_ms = 100;
	std::optional<agi::fs::path> trace_dir;
};

struct PlaybackProbeResult {
	int exit_code = 0;
	bool passed = false;
	bool skip_audio = false;
	int performed_seeks = 0;
	int audio_timer_samples = 0;
	int seek_samples = 0;
	int max_abs_delta_ms = 0;
	double mean_abs_delta_ms = 0.0;
	agi::fs::path trace_dir;
	std::string message;
	std::string selected_video_provider;
	std::string selected_audio_provider;
	std::string actual_video_provider;
	std::string actual_video_decoder;
	bool video_provider_fallback = false;
	std::string video_provider_fallback_reason;
	std::string video_provider_attempts;
	std::string actual_audio_provider_factory;
	std::string actual_audio_provider;
	bool audio_provider_fallback = false;
	std::string audio_provider_fallback_reason;
	std::string audio_provider_attempts;
};

struct CommandLineParseResult {
	bool requested = false;
	std::optional<PlaybackProbeRequest> request;
	std::string error;
};

CommandLineParseResult ParseCommandLine(std::vector<std::string> const& args);
void RunAsync(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done);
std::string Usage();

}
