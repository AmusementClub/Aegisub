#include "resample_dialog_policy.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"

#include <cmath>
#include <cstring>

ResampleDialogReference BuildResampleDialogReference(AssFile const& file, AsyncVideoProvider const* provider) {
	ResampleDialogReference reference;
	file.GetResolution(ScriptResolutionType::PlayRes, reference.script_w, reference.script_h);
	reference.script_matrix = MatrixFromString(file.GetScriptInfo("YCbCr Matrix"));

	if (provider) {
		reference.has_video = true;
		reference.video_w = provider->GetWidth();
		reference.video_h = provider->GetHeight();
		reference.video_matrix = MatrixFromString(provider->GetRealColorSpace());
	}
	else {
		reference.video_w = reference.script_w;
		reference.video_h = reference.script_h;
		reference.video_matrix = YCbCrMatrix::rgb;
	}

	return reference;
}

void InitializeResampleSettings(ResampleSettings& settings, ResampleDialogReference const& reference) {
	std::memset(&settings, 0, sizeof(settings));
	settings.source_x = reference.script_w;
	settings.source_y = reference.script_h;
	settings.source_matrix = reference.script_matrix;
	settings.dest_x = reference.video_w;
	settings.dest_y = reference.video_h;
	settings.dest_matrix = reference.has_video ? reference.video_matrix : reference.script_matrix;
}

bool ResampleAspectRatioChanged(int source_x, int source_y, int dest_x, int dest_y) {
	if (source_x <= 0 || source_y <= 0 || dest_x <= 0 || dest_y <= 0)
		return false;

	auto source_ar = double(source_x) / source_y;
	auto dest_ar = double(dest_x) / dest_y;
	return std::abs(source_ar - dest_ar) / dest_ar > .01;
}

ResampleDialogButtonState BuildResampleDialogButtonState(ResampleDialogReference const& reference,
                                                         ResampleSettings const& settings,
                                                         bool symmetrical) {
	ResampleDialogButtonState state;
	state.from_video_enabled = reference.has_video
		&& (settings.dest_x != reference.video_w || settings.dest_y != reference.video_h);
	state.from_script_enabled = settings.source_x != reference.script_w || settings.source_y != reference.script_h;
	state.ar_mode_enabled = ResampleAspectRatioChanged(settings.source_x, settings.source_y, settings.dest_x, settings.dest_y);

	bool const margins = state.ar_mode_enabled && settings.ar_mode == ResampleARMode::Manual;
	state.symmetrical_enabled = margins;
	state.margin_left_enabled = margins;
	state.margin_top_enabled = margins;
	state.margin_right_enabled = margins && !symmetrical;
	state.margin_bottom_enabled = margins && !symmetrical;
	return state;
}

bool HasLayoutResSensitiveTags(AssFile const& file) {
	for (auto const& line : file.Events) {
		if (line.Comment)
			continue;

		auto const& text = line.Text.get();
		if (text.find("\\frx") != std::string::npos)
			return true;
		if (text.find("\\fry") != std::string::npos)
			return true;
		if (text.find("\\be") != std::string::npos)
			return true;
	}
	return false;
}
