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
#include "video_renderer_opengl_tile.h"
#include "source_frame.h"
#include "subtitle_overlay.h"

#include <memory>
#include <vector>

class OpenGLVideoRenderer final : public IVideoRenderer {
	struct Functions;

	struct Vertex {
		float position[2];
		float texcoord[2];
	};

	struct LayerResources {
		OpenGLVideoRendererTileLayout layout;
		std::vector<GLuint> texture_ids;
		std::vector<Vertex> vertices;
		std::vector<GLuint> indices;
		GLuint vertex_buffer = 0;
		GLuint element_buffer = 0;
		int canvas_width = 0;
		int canvas_height = 0;
		int offset_x = 0;
		int offset_y = 0;
		SubtitleOverlayCompositionMode composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;
		bool has_content = false;
	};

	std::unique_ptr<Functions> functions;
	LayerResources video_layer;
	LayerResources overlay_layer;

	GLuint program = 0;
	GLint projection_matrix_uniform = -1;
	GLint texture_uniform = -1;

	int max_texture_size = 0;
	bool supports_rectangular_textures = false;
	GLint internal_format = 0;
	bool render_video_layer = true;
	bool render_overlay_layer = true;
	bool clear_before_render = true;

	void EnsureInitialized();
	void LoadFunctions();
	void DetectOpenGLCapabilities();
	void CreateProgram();
	void CreateLayerBuffers(LayerResources& layer);
	void RebuildLayerGeometry(LayerResources& layer);
	void RecreateLayerTextures(LayerResources& layer);
	void UploadBgraLayer(LayerResources& layer, unsigned char const* data, int width, int height, ptrdiff_t pitch, bool flipped, int canvas_width, int canvas_height, int offset_x, int offset_y, SubtitleOverlayCompositionMode composition_mode);
	void UploadDirtyRects(LayerResources& layer, unsigned char const* data, ptrdiff_t pitch, SubtitleOverlayDirtyRect const* dirty_rects, int dirty_rect_count);
	void ClearLayer(LayerResources& layer) noexcept;
	void HideLayer(LayerResources& layer) noexcept;
	void RenderLayer(LayerResources& layer);
	void DestroyResources() noexcept;
	void DeleteLayerTextures(LayerResources& layer) noexcept;

public:
	OpenGLVideoRenderer(bool render_video = true, bool render_overlay = true, bool clear_before_render = true);
	~OpenGLVideoRenderer();

	bool SupportsDirectOverlay() const noexcept override { return render_overlay_layer; }
	std::vector<SourceFramePixelFormat> GetPreferredSourceFormats() const override {
		return { SourceFramePixelFormat::Bgra8 };
	}
	void Reset() override;
	void UploadFrame(SourceFrame const& frame) override;
	void UploadOverlay(SubtitleOverlay const* overlay) override;
	void Render(RenderViewport const& viewport, int canvas_width, int canvas_height) override;
};
