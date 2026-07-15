#pragma once

#include "skia_video_overlay_bounds.h"

void CompositeSkiaVideoOverlayTextures(
	unsigned int normal_texture,
	bool draw_normal,
	SkiaOverlayDeviceBounds const& normal_bounds,
	unsigned int invert_texture,
	bool draw_invert,
	SkiaOverlayDeviceBounds const& invert_bounds,
	int canvas_width,
	int canvas_height);
