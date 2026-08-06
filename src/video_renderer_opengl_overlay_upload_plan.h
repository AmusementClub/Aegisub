// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "subtitle_overlay.h"

#include <algorithm>
#include <cstdint>
#include <limits>

enum class OpenGLVideoRendererOverlayUploadAction {
	HideKeepResources,
	FullUpload,
	DirtyUpload,
	ReuseExistingContent
};

struct OpenGLVideoRendererOverlayLayerState {
	int width = 0;
	int height = 0;
	int canvas_width = 0;
	int canvas_height = 0;
	int offset_x = 0;
	int offset_y = 0;
	bool flipped = false;
	bool has_allocated_resources = false;
	bool has_visible_content = false;
	uint64_t continuity_generation = 0;
	SubtitleOverlayCompositionMode composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;
};

struct OpenGLVideoRendererOverlayUploadPlan {
	OpenGLVideoRendererOverlayUploadAction action = OpenGLVideoRendererOverlayUploadAction::HideKeepResources;
	OpenGLVideoRendererOverlayLayerState next_state = { };
};

inline bool IsValidDirectRenderableOverlayForOpenGLVideoRenderer(SubtitleOverlay const* overlay) {
	return overlay &&
		overlay->IsValid() &&
		overlay->IsDirectRenderable() &&
		overlay->pixel_format == SubtitleOverlayPixelFormat::Bgra8;
}

inline OpenGLVideoRendererOverlayUploadPlan DecideOpenGLVideoRendererOverlayUploadPlan(
	OpenGLVideoRendererOverlayLayerState const& state,
	SubtitleOverlay const* overlay) {
	OpenGLVideoRendererOverlayUploadPlan plan;
	plan.next_state = state;

	if (!IsValidDirectRenderableOverlayForOpenGLVideoRenderer(overlay)) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::HideKeepResources;
		plan.next_state.has_visible_content = false;
		return plan;
	}

	if (!overlay->has_visible_content) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::HideKeepResources;
		plan.next_state.has_visible_content = false;
		return plan;
	}

	bool layout_matches =
		state.has_allocated_resources &&
		state.width == overlay->width &&
		state.height == overlay->height &&
		state.flipped == overlay->flipped &&
		state.canvas_width == overlay->canvas_width &&
		state.canvas_height == overlay->canvas_height &&
		state.offset_x == overlay->target_x &&
		state.offset_y == overlay->target_y &&
		state.composition_mode == overlay->composition_mode;

	plan.next_state.width = overlay->width;
	plan.next_state.height = overlay->height;
	plan.next_state.canvas_width = overlay->canvas_width;
	plan.next_state.canvas_height = overlay->canvas_height;
	plan.next_state.offset_x = overlay->target_x;
	plan.next_state.offset_y = overlay->target_y;
	plan.next_state.flipped = overlay->flipped;
	plan.next_state.composition_mode = overlay->composition_mode;
	plan.next_state.has_allocated_resources = true;
	plan.next_state.has_visible_content = true;
	plan.next_state.continuity_generation = overlay->continuity_generation;

	if (overlay->force_full_upload) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::FullUpload;
		return plan;
	}

	if (!layout_matches) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::FullUpload;
		return plan;
	}

	if (state.continuity_generation != overlay->continuity_generation) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::FullUpload;
		return plan;
	}

	// Once a layer has been hidden, the currently allocated GPU texture is no
	// longer guaranteed to match the logical subtitle state seen by the user.
	// Reactivating it must rebuild the full surface rather than reusing stale
	// preserved contents.
	if (!state.has_visible_content) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::FullUpload;
		return plan;
	}

	if (overlay->dirty_rect_count > 0 && overlay->dirty_rects) {
		plan.action = OpenGLVideoRendererOverlayUploadAction::DirtyUpload;
		return plan;
	}

	// Current overlay providers use an empty dirty-rect set to mean pixel content is unchanged.
	plan.action = OpenGLVideoRendererOverlayUploadAction::ReuseExistingContent;
	return plan;
}

inline int EstimateOpenGLVideoRendererOverlayUploadBytes(
	OpenGLVideoRendererOverlayUploadAction action,
	SubtitleOverlay const* overlay) {
	if (!overlay)
		return 0;

	int64_t const overlay_width = std::max(0, overlay->width);
	int64_t const overlay_height = std::max(0, overlay->height);
	int64_t pixels = 0;
	if (action == OpenGLVideoRendererOverlayUploadAction::FullUpload) {
		pixels = overlay_width * overlay_height;
	}
	else if (action == OpenGLVideoRendererOverlayUploadAction::DirtyUpload
		&& overlay->dirty_rects
		&& overlay->dirty_rect_count > 0) {
		for (int index = 0; index < overlay->dirty_rect_count; ++index) {
			auto const& rect = overlay->dirty_rects[index];
			int64_t const x0 = std::clamp<int64_t>(rect.x, 0, overlay_width);
			int64_t const y0 = std::clamp<int64_t>(rect.y, 0, overlay_height);
			int64_t const x1 = std::clamp<int64_t>(static_cast<int64_t>(rect.x) + rect.width, 0, overlay_width);
			int64_t const y1 = std::clamp<int64_t>(static_cast<int64_t>(rect.y) + rect.height, 0, overlay_height);
			pixels += std::max<int64_t>(0, x1 - x0) * std::max<int64_t>(0, y1 - y0);
			if (pixels >= std::numeric_limits<int>::max() / 4)
				return std::numeric_limits<int>::max();
		}
	}

	if (pixels >= std::numeric_limits<int>::max() / 4)
		return std::numeric_limits<int>::max();
	return static_cast<int>(pixels * 4);
}
