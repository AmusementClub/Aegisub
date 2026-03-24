#include "video_property_update.h"

#include "ass_file.h"
#include "resolution_resampler.h"

#include <cmath>

namespace {
bool resolution_matches_video_multiple(VideoPropertyUpdateInput const& input) {
	return input.video_width > 0
		&& input.video_height > 0
		&& input.script_width % input.video_width == 0
		&& input.script_height % input.video_height == 0;
}
}

VideoPropertyUpdatePlan PlanVideoPropertyUpdate(VideoPropertyUpdateInput const& input) {
	VideoPropertyUpdatePlan plan;
	plan.update_matrix = input.set_properties && input.provider_matrix != input.current_matrix;

	if (input.resolution_type == ScriptResolutionType::None) {
		plan.set_resolution = true;
		return plan;
	}

	if (!input.set_properties)
		return plan;

	if (input.script_width <= 0 || input.script_height <= 0 || input.video_width <= 0 || input.video_height <= 0)
		return plan;

	if (resolution_matches_video_multiple(input))
		return plan;

	auto script_aspect_ratio = double(input.script_width) / input.script_height;
	auto video_aspect_ratio = double(input.video_width) / input.video_height;
	plan.aspect_ratio_changed = std::abs(script_aspect_ratio - video_aspect_ratio) / video_aspect_ratio > .01;

	switch (input.mismatch_mode) {
	case VideoResolutionMismatchMode::Ignore:
	default:
		return plan;

	case VideoResolutionMismatchMode::Set:
		plan.set_resolution = true;
		return plan;

	case VideoResolutionMismatchMode::Resample:
		if (!plan.aspect_ratio_changed) {
			plan.resample_mode = ResampleARMode::Stretch;
			return plan;
		}
		break;

	case VideoResolutionMismatchMode::Prompt:
		break;
	}

	plan.prompt_for_resolution_mismatch = true;
	return plan;
}

std::optional<VideoResolutionMismatchChoice> ParseVideoResolutionMismatchChoice(int selection, bool aspect_ratio_changed) {
	switch (selection) {
	case 0:
		return VideoResolutionMismatchChoice::SetScriptResolution;
	case 1:
		return VideoResolutionMismatchChoice::ResampleStretch;
	case 2:
		return aspect_ratio_changed ? std::optional<VideoResolutionMismatchChoice>(VideoResolutionMismatchChoice::ResampleAddBorder) : std::nullopt;
	case 3:
		return aspect_ratio_changed ? std::optional<VideoResolutionMismatchChoice>(VideoResolutionMismatchChoice::ResampleRemoveBorder) : std::nullopt;
	default:
		return std::nullopt;
	}
}

VideoPropertyUpdatePlan ResolveVideoResolutionMismatchChoice(VideoPropertyUpdatePlan plan, VideoResolutionMismatchChoice choice) {
	plan.prompt_for_resolution_mismatch = false;
	switch (choice) {
	case VideoResolutionMismatchChoice::SetScriptResolution:
		plan.set_resolution = true;
		plan.resample_mode.reset();
		break;
	case VideoResolutionMismatchChoice::ResampleStretch:
		plan.set_resolution = false;
		plan.resample_mode = ResampleARMode::Stretch;
		break;
	case VideoResolutionMismatchChoice::ResampleAddBorder:
		plan.set_resolution = false;
		plan.resample_mode = ResampleARMode::AddBorder;
		break;
	case VideoResolutionMismatchChoice::ResampleRemoveBorder:
		plan.set_resolution = false;
		plan.resample_mode = ResampleARMode::RemoveBorder;
		break;
	}
	return plan;
}

void ApplyVideoPropertyUpdatePlan(AssFile *file, VideoPropertyUpdateInput const& input, VideoPropertyUpdatePlan const& plan) {
	if (plan.update_matrix)
		file->SetScriptInfo("YCbCr Matrix", input.provider_matrix);

	if (plan.set_resolution) {
		file->SetResolution(ScriptResolutionType::None, input.video_width, input.video_height);
		return;
	}

	if (plan.resample_mode) {
		ResampleResolution(file, {
			{0, 0, 0, 0},
			input.script_width, input.script_height, input.video_width, input.video_height,
			*plan.resample_mode,
			YCbCrMatrix::rgb, YCbCrMatrix::rgb
		});
	}
}
