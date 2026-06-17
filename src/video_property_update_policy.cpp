#include "video_property_update.h"

#include "resolution_resampler.h"

#include "ass_file.h"

#include <cmath>

namespace {
bool resolution_matches_video_multiple(VideoPropertyUpdateInput const& input) {
	return input.video_width > 0
		&& input.video_height > 0
		&& input.script_width % input.video_width == 0
		&& input.script_height % input.video_height == 0;
}

bool aspect_ratios_match(int w1, int h1, int w2, int h2) {
	if (w1 <= 0 || h1 <= 0 || w2 <= 0 || h2 <= 0)
		return false;
	auto ar1 = double(w1) / h1;
	auto ar2 = double(w2) / h2;
	return std::abs(ar1 - ar2) / ar2 <= .01;
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

	bool const missing_layout_res = input.layout_res_x <= 0 || input.layout_res_y <= 0;
	if (missing_layout_res && input.video_width > 0 && input.video_height > 0)
		plan.prompt_for_layout_res = true;

	if (input.script_width <= 0 || input.script_height <= 0 || input.video_width <= 0 || input.video_height <= 0)
		return plan;

	if (resolution_matches_video_multiple(input))
		return plan;

	// If LayoutRes is set and its aspect ratio matches the video's storage
	// aspect ratio, the PlayRes mismatch is intentional (script was tagged
	// with LayoutRes then resampled to a different PlayRes). Suppress the
	// mismatch prompt; see libass discussion #734 (TheOneric).
	if (input.layout_res_x > 0 && input.layout_res_y > 0
		&& aspect_ratios_match(input.layout_res_x, input.layout_res_y,
		                       input.video_width, input.video_height))
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
