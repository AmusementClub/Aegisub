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
struct pl_dovi_metadata;
struct pl_log_t;
struct pl_opengl_t;
struct pl_frame;
struct pl_renderer_t;
struct pl_tex_t;

namespace placebo { namespace runtime { struct Api; } }

class PlaceboRendererGL final : public IVideoRenderer {
	struct Functions;
	struct UploadedPlaneState {
		pl_tex_t const* texture = nullptr;
		int components = 0;
		std::array<int, 4> component_mapping = { { -1, -1, -1, -1 } };
		bool flipped = false;
		float shift_x = 0.0f;
		float shift_y = 0.0f;
		size_t estimated_bytes = 0;
	};

	placebo::runtime::Api const* api = nullptr;
	std::unique_ptr<Functions> functions;
	pl_log_t const* log = nullptr;
	pl_opengl_t const* opengl = nullptr;
	pl_renderer_t* renderer = nullptr;
	std::array<UploadedPlaneState, 4> image_planes = { };
	std::array<pl_tex_t const*, 4> image_avframe_textures = { };
	std::unique_ptr<pl_frame> mapped_avframe;
	pl_tex_t const* target_texture = nullptr;
	unsigned int target_gl_texture = 0;
	unsigned int target_render_framebuffer = 0;
	int image_width = 0;
	int image_height = 0;
	int image_plane_count = 0;
	SourceFrameOutputMode image_output_mode = SourceFrameOutputMode::Bgra8;
	SourceFrameFormatInfo image_format_info;
	SourceFrameColorMetadata image_color;
	SourceFrameDolbyVisionMetadata image_dolby_vision;
	SourceFrameChromaLocation image_chroma_location = SourceFrameChromaLocation::Unknown;
	SourceFrameGeometry image_geometry;
	std::unique_ptr<pl_dovi_metadata> image_placebo_dovi_metadata;
	size_t image_avframe_texture_estimated_bytes = 0;
	int target_width = 0;
	int target_height = 0;
	unsigned int target_framebuffer = 0;
	size_t target_texture_estimated_bytes = 0;
	bool has_frame = false;

	void EnsureInitialized();
	void DestroyResources() noexcept;
	void DestroyImageResources() noexcept;
	void DestroyMappedAVFrame() noexcept;
	void DestroyPlaneResources() noexcept;
	void DestroyAVFrameTextures() noexcept;
	void DestroyTargetResources() noexcept;
	void RecreateTargetTexture(int canvas_width, int canvas_height);

public:
	PlaceboRendererGL();
	~PlaceboRendererGL() override;

	bool SupportsDirectOverlay() const noexcept override { return false; }
	bool PrefersSceneCacheForRepaint() const noexcept override { return true; }
	char const* GetDebugName() const noexcept override { return "libplacebo"; }
	size_t EstimateTextureBytes() const noexcept override;
	std::vector<SourceFrameOutputMode> GetPreferredSourceModes() const override {
		return { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	}
	void Reset() override;
	void UploadFrame(SourceFrame const& frame) override;
	void UploadOverlay(SubtitleOverlay const* overlay) override;
	void Render(RenderViewport const& viewport, int canvas_width, int canvas_height) override;
};
