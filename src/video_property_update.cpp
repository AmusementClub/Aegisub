#include "video_property_update.h"

#include "ass_file.h"
#include "async_video_provider.h"
#include "resolution_resampler.h"

VideoPropertyUpdateInput BuildVideoPropertyUpdateInput(AssFile const* file,
                                                       AsyncVideoProvider const* provider,
                                                       VideoResolutionMismatchMode mismatch_mode) {
	int sx, sy;
	return {
		provider->ShouldSetVideoProperties(),
		file->GetScriptInfo("YCbCr Matrix"),
		provider->GetColorSpace(),
		file->GetResolutionType(ScriptResolutionType::PlayRes, sx, sy),
		sx,
		sy,
		provider->GetWidth(),
		provider->GetHeight(),
		file->GetScriptInfoAsInt("LayoutResX"),
		file->GetScriptInfoAsInt("LayoutResY"),
		mismatch_mode
	};
}

void ApplyVideoPropertyUpdatePlan(AssFile *file, VideoPropertyUpdateInput const& input, VideoPropertyUpdatePlan const& plan) {
	if (plan.update_matrix)
		file->SetScriptInfo("YCbCr Matrix", input.provider_matrix);

	if (plan.set_resolution) {
		file->SetResolution(ScriptResolutionType::None, input.video_width, input.video_height);
		return;
	}

	if (plan.set_layout_res) {
		file->SetScriptInfo("LayoutResX", std::to_string(input.video_width));
		file->SetScriptInfo("LayoutResY", std::to_string(input.video_height));
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
