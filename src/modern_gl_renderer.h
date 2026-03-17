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
#include "modern_gl_renderer_tile.h"

#include <memory>
#include <vector>

class ModernGLRenderer final : public IVideoRenderer {
	struct Functions;

	struct Vertex {
		float position[2];
		float texcoord[2];
	};

	std::unique_ptr<Functions> functions;
	ModernGLTileLayout layout;
	std::vector<GLuint> texture_ids;
	std::vector<Vertex> vertices;
	std::vector<GLuint> indices;

	GLuint program = 0;
	GLuint vertex_buffer = 0;
	GLuint element_buffer = 0;
	GLint projection_matrix_uniform = -1;
	GLint texture_uniform = -1;

	int max_texture_size = 0;
	bool supports_rectangular_textures = false;
	GLint internal_format = 0;
	bool has_frame = false;

	void EnsureInitialized();
	void LoadFunctions();
	void DetectOpenGLCapabilities();
	void CreateProgram();
	void CreateBuffers();
	void RebuildLayout(VideoFrame const& frame);
	void RebuildGeometry();
	void RecreateTextures();
	void DestroyResources() noexcept;
	void DeleteTextures() noexcept;

public:
	ModernGLRenderer();
	~ModernGLRenderer();

	void Reset() override;
	void UploadFrame(VideoFrame const& frame) override;
	void Render(RenderViewport const& viewport, int canvas_width, int canvas_height) override;
};
