// Copyright (c) 2026
// All rights reserved.

#pragma once

#include <vector>

#include <wx/gdicmn.h>

class AudioRenderer;
class wxDC;

struct AudioViewportRequest {
	int scroll_left = 0;
	double ms_per_pixel = 1.0;
	int audio_top = 0;
	int audio_height = 0;
	int foot_size = 0;
	wxRect update_rect;
	int begin_ms = 0;
	int end_ms = 0;
};

class AudioTileCompositor {
public:
	void Compose(wxDC &dc,
		AudioRenderer &renderer,
		const AudioViewportRequest &viewport,
		const std::vector<std::pair<int, int>> &style_ranges) const;
};
