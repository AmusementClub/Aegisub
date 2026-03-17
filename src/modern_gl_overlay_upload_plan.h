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

enum class ModernGLOverlayUploadAction {
	HideKeepResources,
	FullUpload,
	DirtyUpload,
	ReuseExistingContent
};

struct ModernGLOverlayLayerState {
	int width = 0;
	int height = 0;
	int canvas_width = 0;
	int canvas_height = 0;
	int offset_x = 0;
	int offset_y = 0;
	bool flipped = false;
	bool has_allocated_resources = false;
	bool has_visible_content = false;
	SubtitleOverlayCompositionMode composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;
};

struct ModernGLOverlayUploadPlan {
	ModernGLOverlayUploadAction action = ModernGLOverlayUploadAction::HideKeepResources;
	ModernGLOverlayLayerState next_state = { };
};

inline bool IsValidDirectRenderableOverlayForModernGL(SubtitleOverlay const* overlay) {
	return overlay &&
		overlay->IsValid() &&
		overlay->IsDirectRenderable() &&
		overlay->pixel_format == SubtitleOverlayPixelFormat::Bgra8;
}

inline ModernGLOverlayUploadPlan DecideModernGLOverlayUploadPlan(
	ModernGLOverlayLayerState const& state,
	SubtitleOverlay const* overlay) {
	ModernGLOverlayUploadPlan plan;
	plan.next_state = state;

	if (!IsValidDirectRenderableOverlayForModernGL(overlay)) {
		plan.action = ModernGLOverlayUploadAction::HideKeepResources;
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

	if (overlay->force_full_upload) {
		plan.action = ModernGLOverlayUploadAction::FullUpload;
		return plan;
	}

	if (!layout_matches) {
		plan.action = ModernGLOverlayUploadAction::FullUpload;
		return plan;
	}

	if (overlay->dirty_rect_count > 0 && overlay->dirty_rects) {
		plan.action = ModernGLOverlayUploadAction::DirtyUpload;
		return plan;
	}

	// Current overlay providers use an empty dirty-rect set to mean pixel content is unchanged.
	plan.action = ModernGLOverlayUploadAction::ReuseExistingContent;
	return plan;
}
