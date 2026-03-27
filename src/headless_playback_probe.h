#pragma once

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>

class wxArrayString;

namespace headless_playback_probe {

struct Options {
	agi::fs::path video_path;
	agi::fs::path audio_path;
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	bool skip_audio = false;
	int duration_ms = 2000;
	double audio_rate_scale = 0.9;
	int audio_quantum_ms = 0;
	int max_allowed_abs_delta_ms = 100;
	std::optional<agi::fs::path> trace_dir;
};

struct ParseResult {
	bool requested = false;
	std::optional<Options> options;
	std::string error;
};

ParseResult Parse(wxArrayString const& args);
void RunAsync(Options options, std::function<void(int)> on_done);
std::string Usage();

}
