// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include <algorithm>

struct SecondarySubtitleStripLayout {
	int source_top = 0;
	int source_height = 0;
	int max_scroll_offset_y = 0;
	int clamped_scroll_offset_y = 0;
};

inline bool ShouldShowSecondarySubtitleStrip(
	bool has_video,
	bool enabled,
	bool detached_video_box,
	bool detached_mode) {
	return has_video && enabled && detached_video_box == detached_mode;
}

inline SecondarySubtitleStripLayout BuildSecondarySubtitleStripLayout(
	int content_height,
	int visible_source_height,
	int scroll_offset_y) {
	SecondarySubtitleStripLayout layout;
	if (content_height <= 0 || visible_source_height <= 0)
		return layout;

	layout.source_height = std::min(content_height, visible_source_height);
	layout.max_scroll_offset_y = std::max(content_height - layout.source_height, 0);
	layout.clamped_scroll_offset_y = std::clamp(scroll_offset_y, 0, layout.max_scroll_offset_y);
	layout.source_top = std::max(content_height - layout.source_height - layout.clamped_scroll_offset_y, 0);
	return layout;
}

inline int SecondarySubtitleStripThumbPositionFromScrollOffset(int scroll_offset_y, int max_scroll_offset_y) {
	int max_offset = std::max(max_scroll_offset_y, 0);
	int clamped_scroll_offset_y = std::clamp(scroll_offset_y, 0, max_offset);
	return std::clamp(max_offset - clamped_scroll_offset_y, 0, max_offset);
}

inline int SecondarySubtitleStripScrollOffsetFromThumbPosition(int thumb_position, int max_scroll_offset_y) {
	int max_offset = std::max(max_scroll_offset_y, 0);
	int clamped_thumb_position = std::clamp(thumb_position, 0, max_offset);
	return std::clamp(max_offset - clamped_thumb_position, 0, max_offset);
}
