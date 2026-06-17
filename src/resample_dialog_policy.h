#pragma once

#include "resolution_resampler.h"

class AssFile;
class AsyncVideoProvider;

struct ResampleDialogReference {
	int script_w = 0;
	int script_h = 0;
	YCbCrMatrix script_matrix = YCbCrMatrix::rgb;
	int video_w = 0;
	int video_h = 0;
	YCbCrMatrix video_matrix = YCbCrMatrix::rgb;
	bool has_video = false;
};

struct ResampleDialogButtonState {
	bool from_video_enabled = false;
	bool from_script_enabled = false;
	bool ar_mode_enabled = false;
	bool symmetrical_enabled = false;
	bool margin_left_enabled = false;
	bool margin_top_enabled = false;
	bool margin_right_enabled = false;
	bool margin_bottom_enabled = false;
};

ResampleDialogReference BuildResampleDialogReference(AssFile const& file, AsyncVideoProvider const* provider);
void InitializeResampleSettings(ResampleSettings& settings, ResampleDialogReference const& reference);
bool ResampleAspectRatioChanged(int source_x, int source_y, int dest_x, int dest_y);
ResampleDialogButtonState BuildResampleDialogButtonState(ResampleDialogReference const& reference,
                                                         ResampleSettings const& settings,
                                                         bool symmetrical);
bool HasLayoutResSensitiveTags(AssFile const& file);
