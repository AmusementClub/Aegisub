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

#include "headless_playback_probe.h"

#include <libaegisub/fs.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace headless_playback_probe {
namespace {

std::optional<int> ParseInt(std::string const& text) {
	try {
		size_t consumed = 0;
		int value = std::stoi(text, &consumed);
		if (consumed != text.size())
			return std::nullopt;
		return value;
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<double> ParseDouble(std::string const& text) {
	try {
		size_t consumed = 0;
		double value = std::stod(text, &consumed);
		if (consumed != text.size())
			return std::nullopt;
		return value;
	}
	catch (...) {
		return std::nullopt;
	}
}

bool IsRequested(std::vector<std::string> const& args) {
	for (size_t i = 1; i < args.size(); ++i) {
		if (args[i] == "--headless-playback-probe")
			return true;
	}
	return false;
}

}

RequestParseResult ParseRequestArguments(std::vector<std::string> const& args, bool require_video) {
	RequestParseResult result;
	PlaybackProbeRequest request;
	bool have_video = false;

	auto require_value = [&](size_t& index, char const* flag) -> std::optional<std::string> {
		if (index + 1 >= args.size()) {
			result.error = std::string(flag) + " requires a value\n" + Usage();
			return std::nullopt;
		}
		++index;
		return args[index];
	};

	for (size_t i = 1; i < args.size(); ++i) {
		auto const& arg = args[i];
		if (arg == "--headless-playback-probe")
			continue;
		if (arg == "--probe-video") {
			auto value = require_value(i, "--probe-video");
			if (!value)
				return result;
			request.video_path = *value;
			have_video = true;
			continue;
		}
		if (arg == "--probe-audio") {
			auto value = require_value(i, "--probe-audio");
			if (!value)
				return result;
			request.audio_path = *value;
			continue;
		}
		if (arg == "--probe-skip-audio") {
			request.skip_audio = true;
			continue;
		}
		if (arg == "--probe-line-start-ms") {
			auto value = require_value(i, "--probe-line-start-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed < 0) {
				result.error = "--probe-line-start-ms must be a non-negative integer\n" + Usage();
				return result;
			}
			request.line_start_ms = *parsed;
			continue;
		}
		if (arg == "--probe-repeat-count") {
			auto value = require_value(i, "--probe-repeat-count");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed <= 0) {
				result.error = "--probe-repeat-count must be a positive integer\n" + Usage();
				return result;
			}
			request.repeat_count = *parsed;
			continue;
		}
		if (arg == "--probe-repeat-gap-ms") {
			auto value = require_value(i, "--probe-repeat-gap-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed < 0) {
				result.error = "--probe-repeat-gap-ms must be a non-negative integer\n" + Usage();
				return result;
			}
			request.repeat_gap_ms = *parsed;
			continue;
		}
		if (arg == "--probe-seek-after-ms") {
			auto value = require_value(i, "--probe-seek-after-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed < 0) {
				result.error = "--probe-seek-after-ms must be a non-negative integer\n" + Usage();
				return result;
			}
			request.seek_after_ms = *parsed;
			continue;
		}
		if (arg == "--probe-seek-target-offset-ms") {
			auto value = require_value(i, "--probe-seek-target-offset-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed < 0) {
				result.error = "--probe-seek-target-offset-ms must be a non-negative integer\n" + Usage();
				return result;
			}
			request.seek_target_offset_ms = *parsed;
			continue;
		}
		if (arg == "--probe-video-provider") {
			auto value = require_value(i, "--probe-video-provider");
			if (!value)
				return result;
			request.video_provider = *value;
			continue;
		}
		if (arg == "--probe-audio-provider") {
			auto value = require_value(i, "--probe-audio-provider");
			if (!value)
				return result;
			request.audio_provider = *value;
			continue;
		}
		if (arg == "--probe-duration-ms") {
			auto value = require_value(i, "--probe-duration-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed <= 0) {
				result.error = "--probe-duration-ms must be a positive integer\n" + Usage();
				return result;
			}
			request.duration_ms = *parsed;
			continue;
		}
		if (arg == "--probe-audio-rate-scale") {
			auto value = require_value(i, "--probe-audio-rate-scale");
			if (!value)
				return result;
			auto parsed = ParseDouble(*value);
			if (!parsed || *parsed <= 0.0) {
				result.error = "--probe-audio-rate-scale must be a positive number\n" + Usage();
				return result;
			}
			request.audio_rate_scale = *parsed;
			continue;
		}
		if (arg == "--probe-audio-quantum-ms") {
			auto value = require_value(i, "--probe-audio-quantum-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed < 0) {
				result.error = "--probe-audio-quantum-ms must be a non-negative integer\n" + Usage();
				return result;
			}
			request.audio_quantum_ms = *parsed;
			continue;
		}
		if (arg == "--probe-max-abs-delta-ms") {
			auto value = require_value(i, "--probe-max-abs-delta-ms");
			if (!value)
				return result;
			auto parsed = ParseInt(*value);
			if (!parsed || *parsed <= 0) {
				result.error = "--probe-max-abs-delta-ms must be a positive integer\n" + Usage();
				return result;
			}
			request.max_allowed_abs_delta_ms = *parsed;
			continue;
		}
		if (arg == "--probe-trace-dir") {
			auto value = require_value(i, "--probe-trace-dir");
			if (!value)
				return result;
			request.trace_dir = agi::fs::path(*value);
			continue;
		}

		result.error = "unrecognized headless playback probe argument: " + arg + "\n" + Usage();
		return result;
	}

	if (require_video && !have_video) {
		result.error = "--headless-playback-probe requires --probe-video\n" + Usage();
		return result;
	}
	if (request.seek_after_ms.has_value() != request.seek_target_offset_ms.has_value()) {
		result.error = "--probe-seek-after-ms and --probe-seek-target-offset-ms must be used together\n" + Usage();
		return result;
	}
	if (!request.skip_audio && request.audio_path.empty())
		request.audio_path = request.video_path;

	result.request = std::move(request);
	return result;
}

CommandLineParseResult ParseCommandLine(std::vector<std::string> const& args) {
	CommandLineParseResult result;
	result.requested = IsRequested(args);
	if (!result.requested)
		return result;

	std::vector<std::string> probe_args;
	probe_args.reserve(args.size());
	probe_args.emplace_back(args.empty() ? "Aegisub.exe" : args.front());
	for (size_t i = 1; i < args.size(); ++i) {
		if (args[i] != "--headless-playback-probe")
			probe_args.emplace_back(args[i]);
	}

	auto parsed = ParseRequestArguments(probe_args, true);
	result.request = std::move(parsed.request);
	result.error = std::move(parsed.error);
	return result;
}

void RunAsync(PlaybackProbeRequest request, std::function<void(PlaybackProbeResult)> on_done) {
	aegisub::playback_probe_service::RunAsync(std::move(request), std::move(on_done));
}

std::string Usage() {
	return
		"Usage: Aegisub.exe --headless-playback-probe --probe-video <path> "
		"[--probe-audio <path>] [--probe-skip-audio] [--probe-line-start-ms <ms>] "
		"[--probe-repeat-count <count>] [--probe-repeat-gap-ms <ms>] "
		"[--probe-seek-after-ms <ms>] [--probe-seek-target-offset-ms <ms>] "
		"[--probe-video-provider <name>] [--probe-audio-provider <name>] "
		"[--probe-duration-ms <ms>] "
		"[--probe-audio-rate-scale <scale>] [--probe-audio-quantum-ms <ms>] "
		"[--probe-max-abs-delta-ms <ms>] [--probe-trace-dir <path>]";
}

}
