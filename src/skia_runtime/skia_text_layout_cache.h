// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "video_overlay_draw_context.h"

#ifdef WITH_SKIA
#include <include/core/SkRefCnt.h>
#include <include/core/SkTypeface.h>
#endif

#include <string>
#include <unordered_map>

class SkCanvas;

class SkiaTextLayoutCache {
	struct FontKey {
		std::string face;
		int size = 0;
		bool bold = false;
		bool italic = false;

		bool operator==(FontKey const& other) const;
	};

	struct FontKeyHash {
		size_t operator()(FontKey const& key) const noexcept;
	};

#ifdef WITH_SKIA
	std::unordered_map<FontKey, sk_sp<SkTypeface>, FontKeyHash> typefaces;
#endif

	sk_sp<SkTypeface> ResolveTypeface(VideoOverlayTextStyle const& style);

public:
	wxSize MeasureText(std::string const& text, VideoOverlayTextStyle const& style);
	void DrawText(SkCanvas &canvas, std::string const& text, int x, int y, VideoOverlayTextStyle const& style);
};
