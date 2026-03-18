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

#include "ivideo_renderer.h"
#include "source_frame.h"

#include <array>
#include <memory>

struct SubtitleOverlay;
struct pl_log_t;
struct pl_opengl_t;
struct pl_renderer_t;
struct pl_tex_t;

namespace placebo { namespace runtime { struct Api; } }

class PlaceboRendererGL final : public IVideoRenderer {
	struct Functions;

	placebo::runtime::Api const* api = nullptr;
	std::unique_ptr<Functions> functions;
	pl_log_t const* log = nullptr;
	pl_opengl_t const* opengl = nullptr;
	pl_renderer_t* renderer = nullptr;
	pl_tex_t const* image_texture = nullptr;
	pl_tex_t const* target_texture = nullptr;
	int image_width = 0;
	int image_height = 0;
	int image_components = 0;
	std::array<int, 4> image_component_mapping = { { -1, -1, -1, -1 } };
	bool image_flipped = false;
	SourceFrameColorMetadata image_color;
	int target_width = 0;
	int target_height = 0;
	bool has_frame = false;

	void EnsureInitialized();
	void DestroyResources() noexcept;
	void DestroyImageResources() noexcept;
	void DestroyTargetResources() noexcept;
	void RecreateTargetTexture(int canvas_width, int canvas_height);
	void RestoreCompatibilityState() noexcept;

public:
	PlaceboRendererGL();
	~PlaceboRendererGL() override;

	bool SupportsDirectOverlay() const noexcept override { return false; }
	void Reset() override;
	void UploadFrame(SourceFrame const& frame) override;
	void UploadOverlay(SubtitleOverlay const* overlay) override;
	void Render(RenderViewport const& viewport, int canvas_width, int canvas_height) override;
};
