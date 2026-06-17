#pragma once

#include <optional>
#include <string>

class AssFile;
class AsyncVideoProvider;
enum class ScriptResolutionType : int;
enum class ResampleARMode : int;

enum class VideoResolutionMismatchMode : int {
	Ignore = 0,
	Prompt = 1,
	Resample = 2,
	Set = 3
};

enum class VideoResolutionMismatchChoice : int {
	SetScriptResolution = 0,
	ResampleStretch = 1,
	ResampleAddBorder = 2,
	ResampleRemoveBorder = 3
};

struct VideoPropertyUpdateInput {
	bool set_properties = false;
	std::string current_matrix;
	std::string provider_matrix;
	ScriptResolutionType resolution_type;
	int script_width = 0;
	int script_height = 0;
	int video_width = 0;
	int video_height = 0;
	int layout_res_x = 0;
	int layout_res_y = 0;
	VideoResolutionMismatchMode mismatch_mode = VideoResolutionMismatchMode::Ignore;
};

struct VideoPropertyUpdatePlan {
	bool update_matrix = false;
	bool set_resolution = false;
	bool set_layout_res = false;
	std::optional<ResampleARMode> resample_mode;
	bool prompt_for_resolution_mismatch = false;
	bool prompt_for_layout_res = false;
	bool aspect_ratio_changed = false;

	bool ShouldCommit() const {
		return update_matrix || set_resolution || set_layout_res || resample_mode.has_value();
	}
};

VideoPropertyUpdateInput BuildVideoPropertyUpdateInput(AssFile const* file,
                                                       AsyncVideoProvider const* provider,
                                                       VideoResolutionMismatchMode mismatch_mode);
VideoPropertyUpdatePlan PlanVideoPropertyUpdate(VideoPropertyUpdateInput const& input);
std::optional<VideoResolutionMismatchChoice> ParseVideoResolutionMismatchChoice(int selection, bool aspect_ratio_changed);
VideoPropertyUpdatePlan ResolveVideoResolutionMismatchChoice(VideoPropertyUpdatePlan plan, VideoResolutionMismatchChoice choice);
void ApplyVideoPropertyUpdatePlan(AssFile *file, VideoPropertyUpdateInput const& input, VideoPropertyUpdatePlan const& plan);
