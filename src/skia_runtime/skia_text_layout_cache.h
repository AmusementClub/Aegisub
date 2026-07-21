// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "video_overlay_draw_context.h"

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
#include <include/core/SkRefCnt.h>
#include <include/core/SkTypeface.h>
#endif

#include <optional>
#include <string>
#include <unordered_map>

class SkCanvas;

class SkiaTextLayoutCache {
	struct FontKey {
		std::string face;
		int size = 0;
		bool bold = false;
		bool italic = false;

		bool operator==(FontKey const& other) const noexcept;
	};

	struct FontKeyHash {
		size_t operator()(FontKey const& key) const noexcept;
	};

	struct TextMeasureKey {
		std::string text;
		FontKey font;
	};

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	std::unordered_map<FontKey, sk_sp<SkTypeface>, FontKeyHash> typefaces;
	// The recorder measures the same label immediately before recording it. A
	// last-value cache covers that hot path without periodically clearing a large
	// hash table as labels change during interaction.
	std::optional<TextMeasureKey> last_measured_key;
	wxSize last_measured_size;
#endif

	sk_sp<SkTypeface> ResolveTypeface(VideoOverlayTextStyle const& style);

public:
	wxSize MeasureText(std::string const& text, VideoOverlayTextStyle const& style);
	void DrawText(SkCanvas &canvas, std::string const& text, int x, int y, VideoOverlayTextStyle const& style);
};
