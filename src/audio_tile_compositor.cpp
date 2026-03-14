// Copyright (c) 2026
// All rights reserved.

#include "audio_tile_compositor.h"

#include "audio_renderer.h"

#include <algorithm>

namespace {
int RelativeXFromTime(int ms, int scroll_left, double ms_per_pixel) {
	return static_cast<int>(ms / ms_per_pixel) - scroll_left;
}
}

void AudioTileCompositor::Compose(
	wxDC &dc,
	AudioRenderer &renderer,
	const AudioViewportRequest &viewport,
	const std::vector<std::pair<int, int>> &style_ranges) const {
	if (viewport.update_rect.width <= 0 || viewport.audio_height <= 0)
		return;

	auto pt = begin(style_ranges);
	auto pe = end(style_ranges);
	while (pt != pe && pt + 1 != pe && (pt + 1)->first < viewport.begin_ms)
		++pt;

	while (pt != pe && pt->first < viewport.end_ms) {
		const auto range_style = static_cast<AudioRenderingStyle>(pt->second);
		const int range_x1 = std::max(viewport.update_rect.x,
			RelativeXFromTime(pt->first, viewport.scroll_left, viewport.ms_per_pixel));
		int range_x2 = viewport.update_rect.x + viewport.update_rect.width;
		if (++pt != pe) {
			range_x2 = std::min(range_x2,
				RelativeXFromTime(pt->first, viewport.scroll_left, viewport.ms_per_pixel));
		}

		if (range_x2 > range_x1) {
			renderer.Render(dc,
				wxPoint(range_x1, viewport.audio_top),
				range_x1 + viewport.scroll_left,
				range_x2 - range_x1,
				range_style);
		}
	}
}
