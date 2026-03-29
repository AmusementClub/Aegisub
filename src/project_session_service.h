#pragma once

#include "project_query_service.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::project_session_service {

enum class ProjectSessionStepKind {
	OpenMedia,
	ReopenMedia,
	CloseMedia,
	OpenSubtitles,
	OpenSubtitlesUnlinked,
	OpenSubtitlesFromVideo,
	CloseSubtitles,
	OpenTimecodes,
	CloseTimecodes,
	OpenKeyframes,
	CloseKeyframes,
	QueryProject,
	AssertProject,
	AssertSubtitleCounts,
	AssertSubtitleModified
};

struct ProjectSessionStep {
	ProjectSessionStepKind kind = ProjectSessionStepKind::QueryProject;
	bool expected_subtitle_file_loaded = false;
	bool expected_has_video = false;
	bool expected_has_audio = false;
	bool expected_timecodes_file_loaded = false;
	bool expected_keyframes_file_loaded = false;
	bool expected_subtitle_modified = false;
	int primary_value = 0;
	int secondary_value = 0;
	int tertiary_value = 0;
	int quaternary_value = 0;
	std::string source_text;
};

struct ProjectSessionRequest {
	agi::fs::path video_path;
	agi::fs::path audio_path;
	agi::fs::path subtitle_path;
	agi::fs::path timecodes_path;
	agi::fs::path keyframes_path;
	std::string subtitle_encoding;
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	bool skip_audio = false;
	double audio_rate_scale = 1.0;
	int audio_quantum_ms = 0;
	std::optional<agi::fs::path> trace_dir;
	std::vector<ProjectSessionStep> steps;
};

struct ProjectSessionResult {
	int exit_code = 0;
	bool passed = false;
	size_t total_steps = 0;
	size_t completed_steps = 0;
	size_t open_media_count = 0;
	size_t reopen_media_count = 0;
	size_t close_media_count = 0;
	size_t open_subtitles_count = 0;
	size_t close_subtitles_count = 0;
	size_t open_timecodes_count = 0;
	size_t close_timecodes_count = 0;
	size_t open_keyframes_count = 0;
	size_t close_keyframes_count = 0;
	size_t query_count = 0;
	agi::fs::path trace_dir;
	project_query_service::ProjectSessionSnapshot final_project;
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

void RunAsync(ProjectSessionRequest request, std::function<void(ProjectSessionResult)> on_done);

}
