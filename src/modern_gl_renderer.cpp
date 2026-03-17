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

#include "modern_gl_renderer.h"

#include "video_frame.h"
#include "video_renderer_error.h"

#include <libaegisub/compiler.h>
#include <libaegisub/log.h>

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef __WIN32__
#include <windows.h>
#elif !defined(__APPLE__)
#include <GL/glx.h>
#else
#include <dlfcn.h>
#endif

namespace {
template<typename Exception>
AGI_NOINLINE void throw_error(GLenum err, const char *msg) {
	LOG_E("video/out/modern_gl") << msg << " failed with error code " << err;
	throw Exception(msg, err);
}

template<typename Exception>
AGI_NOINLINE void throw_message(char const *msg) {
	LOG_E("video/out/modern_gl") << msg;
	throw Exception(msg);
}
}

#define DO_CHECK_ERROR(cmd, Exception, msg) \
	do { \
		cmd; \
		GLenum err = glGetError(); \
		AGI_UNLIKELY_IF(err) \
			throw_error<Exception>(err, msg); \
	} while (0)

#define CHECK_INIT_ERROR(cmd) DO_CHECK_ERROR(cmd, VideoOutInitException, #cmd)
#define CHECK_RENDER_ERROR(cmd) DO_CHECK_ERROR(cmd, VideoOutRenderException, #cmd)

struct ModernGLRenderer::Functions {
	PFNGLATTACHSHADERPROC AttachShader = nullptr;
	PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLBUFFERDATAPROC BufferData = nullptr;
	PFNGLCOMPILESHADERPROC CompileShader = nullptr;
	PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
	PFNGLCREATESHADERPROC CreateShader = nullptr;
	PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
	PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
	PFNGLDELETESHADERPROC DeleteShader = nullptr;
	PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray = nullptr;
	PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray = nullptr;
	PFNGLGENBUFFERSPROC GenBuffers = nullptr;
	PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
	PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
	PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
	PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
	PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
	PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
	PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
	PFNGLUNIFORM1IPROC Uniform1i = nullptr;
	PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv = nullptr;
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer = nullptr;
};

namespace {
void *GetGLProcAddress(char const *name) {
#ifdef __WIN32__
	void *proc = reinterpret_cast<void *>(wglGetProcAddress(name));
	if (proc == nullptr || proc == reinterpret_cast<void *>(0x1) || proc == reinterpret_cast<void *>(0x2) || proc == reinterpret_cast<void *>(0x3) || proc == reinterpret_cast<void *>(-1))
		return nullptr;
	return proc;
#elif defined(__APPLE__)
	return dlsym(RTLD_DEFAULT, name);
#else
	return reinterpret_cast<void *>(glXGetProcAddress(reinterpret_cast<GLubyte const *>(name)));
#endif
}

template<typename Proc>
void LoadProc(Proc& proc, char const *name) {
	proc = reinterpret_cast<Proc>(GetGLProcAddress(name));
	if (!proc)
		throw_message<VideoOutInitException>(name);
}

template<typename Exception>
AGI_NOINLINE void throw_shader_message(char const *label, std::string const& log) {
	LOG_E("video/out/modern_gl") << label << ": " << log;
	throw Exception(label);
}

bool TestTexture(int width, int height, GLint format) {
	glTexImage2D(GL_PROXY_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
	while (glGetError()) { }
	LOG_I("video/out/modern_gl") << "ModernGLRenderer::TestTexture " << width << "x" << height;
	return format != 0;
}
}

ModernGLRenderer::ModernGLRenderer() = default;

ModernGLRenderer::~ModernGLRenderer() {
	DestroyResources();
}

void ModernGLRenderer::LoadFunctions() {
	if (functions)
		return;

	functions = std::make_unique<Functions>();
	LoadProc(functions->AttachShader, "glAttachShader");
	LoadProc(functions->BindAttribLocation, "glBindAttribLocation");
	LoadProc(functions->BindBuffer, "glBindBuffer");
	LoadProc(functions->BufferData, "glBufferData");
	LoadProc(functions->CompileShader, "glCompileShader");
	LoadProc(functions->CreateProgram, "glCreateProgram");
	LoadProc(functions->CreateShader, "glCreateShader");
	LoadProc(functions->DeleteBuffers, "glDeleteBuffers");
	LoadProc(functions->DeleteProgram, "glDeleteProgram");
	LoadProc(functions->DeleteShader, "glDeleteShader");
	LoadProc(functions->DisableVertexAttribArray, "glDisableVertexAttribArray");
	LoadProc(functions->EnableVertexAttribArray, "glEnableVertexAttribArray");
	LoadProc(functions->GenBuffers, "glGenBuffers");
	LoadProc(functions->GetProgramInfoLog, "glGetProgramInfoLog");
	LoadProc(functions->GetProgramiv, "glGetProgramiv");
	LoadProc(functions->GetShaderInfoLog, "glGetShaderInfoLog");
	LoadProc(functions->GetShaderiv, "glGetShaderiv");
	LoadProc(functions->GetUniformLocation, "glGetUniformLocation");
	LoadProc(functions->LinkProgram, "glLinkProgram");
	LoadProc(functions->ShaderSource, "glShaderSource");
	LoadProc(functions->Uniform1i, "glUniform1i");
	LoadProc(functions->UniformMatrix4fv, "glUniformMatrix4fv");
	LoadProc(functions->UseProgram, "glUseProgram");
	LoadProc(functions->VertexAttribPointer, "glVertexAttribPointer");
}

void ModernGLRenderer::DetectOpenGLCapabilities() {
	if (max_texture_size != 0)
		return;

	if (TestTexture(64, 64, GL_RGBA8)) internal_format = GL_RGBA8;
	else if (TestTexture(64, 64, GL_RGBA)) internal_format = GL_RGBA;
	else throw_message<VideoOutInitException>("Could not create a 64x64 RGB texture in any format.");

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	while (max_texture_size > 64 && !TestTexture(max_texture_size, max_texture_size, internal_format))
		max_texture_size >>= 1;
	if (max_texture_size <= 0)
		throw_message<VideoOutInitException>("Could not determine a valid maximum texture size.");

	supports_rectangular_textures = TestTexture(max_texture_size, max_texture_size >> 1, internal_format);
	LOG_I("video/out/modern_gl") << "Maximum texture size is " << max_texture_size << "x" << max_texture_size;
	LOG_I("video/out/modern_gl") << "Rectangular textures supported: " << supports_rectangular_textures;
}

void ModernGLRenderer::CreateProgram() {
	if (program)
		return;

	auto& gl = *functions;
	char const *vertex_shader_source =
		"uniform mat4 u_projection;\n"
		"attribute vec2 a_position;\n"
		"attribute vec2 a_texcoord;\n"
		"varying vec2 v_texcoord;\n"
		"void main() {\n"
		"    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);\n"
		"    v_texcoord = a_texcoord;\n"
		"}\n";
	char const *fragment_shader_source =
		"uniform sampler2D u_texture;\n"
		"varying vec2 v_texcoord;\n"
		"void main() {\n"
		"    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
		"}\n";

	auto compile_shader = [&](GLenum type, char const *source, char const *label) -> GLuint {
		GLuint shader = gl.CreateShader(type);
		if (!shader)
			throw_message<VideoOutInitException>(label);

		gl.ShaderSource(shader, 1, &source, nullptr);
		gl.CompileShader(shader);

		GLint compiled = GL_FALSE;
		gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (compiled == GL_FALSE) {
			GLint log_length = 0;
			gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
			std::string info_log(static_cast<size_t>(std::max(log_length, 1)), '\0');
			GLsizei written = 0;
			gl.GetShaderInfoLog(shader, log_length, &written, info_log.data());
			gl.DeleteShader(shader);
			throw_shader_message<VideoOutInitException>(label, info_log);
		}
		return shader;
	};

	GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source, "Failed to compile video vertex shader.");
	GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source, "Failed to compile video fragment shader.");

	program = gl.CreateProgram();
	if (!program) {
		gl.DeleteShader(vertex_shader);
		gl.DeleteShader(fragment_shader);
		throw_message<VideoOutInitException>("Failed to create video shader program.");
	}

	gl.AttachShader(program, vertex_shader);
	gl.AttachShader(program, fragment_shader);
	gl.BindAttribLocation(program, 0, "a_position");
	gl.BindAttribLocation(program, 1, "a_texcoord");
	gl.LinkProgram(program);

	GLint linked = GL_FALSE;
	gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked == GL_FALSE) {
		GLint log_length = 0;
		gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
		std::string info_log(static_cast<size_t>(std::max(log_length, 1)), '\0');
		GLsizei written = 0;
		gl.GetProgramInfoLog(program, log_length, &written, info_log.data());
		gl.DeleteProgram(program);
		program = 0;
		gl.DeleteShader(vertex_shader);
		gl.DeleteShader(fragment_shader);
		throw_shader_message<VideoOutInitException>("Failed to link video shader program.", info_log);
	}

	gl.DeleteShader(vertex_shader);
	gl.DeleteShader(fragment_shader);

	projection_matrix_uniform = gl.GetUniformLocation(program, "u_projection");
	texture_uniform = gl.GetUniformLocation(program, "u_texture");
	if (projection_matrix_uniform < 0 || texture_uniform < 0)
		throw_message<VideoOutInitException>("Failed to locate video shader uniforms.");

	gl.UseProgram(program);
	gl.Uniform1i(texture_uniform, 0);
	gl.UseProgram(0);
}

void ModernGLRenderer::CreateBuffers() {
	if (vertex_buffer && element_buffer)
		return;

	auto& gl = *functions;
	if (!vertex_buffer)
		gl.GenBuffers(1, &vertex_buffer);
	if (!element_buffer)
		gl.GenBuffers(1, &element_buffer);
	if (!vertex_buffer || !element_buffer)
		throw_message<VideoOutInitException>("Failed to create video vertex/index buffers.");
}

void ModernGLRenderer::EnsureInitialized() {
	LoadFunctions();
	DetectOpenGLCapabilities();
	CreateProgram();
	CreateBuffers();
}

void ModernGLRenderer::DeleteTextures() noexcept {
	if (!texture_ids.empty()) {
		glDeleteTextures(static_cast<GLsizei>(texture_ids.size()), texture_ids.data());
		texture_ids.clear();
	}
}

void ModernGLRenderer::DestroyResources() noexcept {
	if (functions) {
		if (vertex_buffer || element_buffer) {
			GLuint buffers[] = { vertex_buffer, element_buffer };
			functions->DeleteBuffers(2, buffers);
			vertex_buffer = 0;
			element_buffer = 0;
		}
		if (program) {
			functions->DeleteProgram(program);
			program = 0;
		}
	}

	DeleteTextures();
	projection_matrix_uniform = -1;
	texture_uniform = -1;
	has_frame = false;
	layout = { };
	vertices.clear();
	indices.clear();
}

void ModernGLRenderer::Reset() {
	DestroyResources();
	functions.reset();
	max_texture_size = 0;
	supports_rectangular_textures = false;
	internal_format = 0;
}

void ModernGLRenderer::RebuildGeometry() {
	vertices.clear();
	indices.clear();

	vertices.reserve(layout.tiles.size() * 4);
	indices.reserve(layout.tiles.size() * 6);

	for (size_t i = 0; i < layout.tiles.size(); ++i) {
		auto const& tile = layout.tiles[i];
		GLuint base = static_cast<GLuint>(vertices.size());

		vertices.push_back({ { tile.x1, tile.y1 }, { tile.u1, tile.v1 } });
		vertices.push_back({ { tile.x2, tile.y1 }, { tile.u2, tile.v1 } });
		vertices.push_back({ { tile.x2, tile.y2 }, { tile.u2, tile.v2 } });
		vertices.push_back({ { tile.x1, tile.y2 }, { tile.u1, tile.v2 } });

		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}

	auto& gl = *functions;
	gl.BindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
	gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(GLuint)), indices.data(), GL_STATIC_DRAW);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void ModernGLRenderer::RecreateTextures() {
	DeleteTextures();
	texture_ids.resize(layout.tiles.size());
	if (texture_ids.empty())
		return;

	CHECK_INIT_ERROR(glGenTextures(static_cast<GLsizei>(texture_ids.size()), texture_ids.data()));
	for (size_t i = 0; i < layout.tiles.size(); ++i) {
		auto const& tile = layout.tiles[i];
		CHECK_INIT_ERROR(glBindTexture(GL_TEXTURE_2D, texture_ids[i]));
		CHECK_INIT_ERROR(glTexImage2D(GL_TEXTURE_2D, 0, internal_format, tile.texture_w, tile.texture_h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
	}
	CHECK_INIT_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
}

void ModernGLRenderer::RebuildLayout(VideoFrame const& frame) {
	ModernGLTileLayout next_layout = BuildModernGLTileLayout(
		static_cast<int>(frame.width),
		static_cast<int>(frame.height),
		4,
		max_texture_size,
		supports_rectangular_textures,
		frame.flipped);

	layout = std::move(next_layout);
	LOG_I("video/out/modern_gl") << "Video size: " << layout.frame_width << "x" << layout.frame_height << ", tiles: " << layout.tiles.size();
	RebuildGeometry();
	RecreateTextures();
}

void ModernGLRenderer::UploadFrame(VideoFrame const& frame) {
	if (frame.width == 0 || frame.height == 0) {
		has_frame = false;
		return;
	}

	EnsureInitialized();

	if (layout.frame_width != static_cast<int>(frame.width)
		|| layout.frame_height != static_cast<int>(frame.height)
		|| layout.flipped != frame.flipped
		|| texture_ids.size() != layout.tiles.size()) {
		RebuildLayout(frame);
	}

	CHECK_RENDER_ERROR(glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(frame.pitch / 4)));
	for (size_t i = 0; i < layout.tiles.size(); ++i) {
		auto const& tile = layout.tiles[i];
		CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, texture_ids[i]));
		CHECK_RENDER_ERROR(glTexSubImage2D(
			GL_TEXTURE_2D,
			0,
			0,
			0,
			tile.source_w,
			tile.source_h,
			GL_BGRA_EXT,
			GL_UNSIGNED_BYTE,
			frame.data.data() + tile.data_offset));
	}
	CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
	CHECK_RENDER_ERROR(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
	has_frame = true;
}

void ModernGLRenderer::Render(RenderViewport const& viewport, int, int) {
	if (!has_frame || viewport.width <= 0 || viewport.height <= 0 || layout.tiles.empty())
		return;

	EnsureInitialized();

	auto& gl = *functions;
	auto projection_matrix = BuildModernGLOrthoMatrix(layout.frame_width, layout.frame_height, layout.flipped);

	CHECK_RENDER_ERROR(glDisable(GL_SCISSOR_TEST));
	CHECK_RENDER_ERROR(glDisable(GL_STENCIL_TEST));
	CHECK_RENDER_ERROR(glDisable(GL_CULL_FACE));
	CHECK_RENDER_ERROR(glDisable(GL_BLEND));
	CHECK_RENDER_ERROR(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
	CHECK_RENDER_ERROR(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
	CHECK_RENDER_ERROR(glClearStencil(0));
	CHECK_RENDER_ERROR(glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
	CHECK_RENDER_ERROR(glViewport(viewport.x, viewport.y, viewport.width, viewport.height));

	gl.UseProgram(program);
	gl.UniformMatrix4fv(projection_matrix_uniform, 1, GL_FALSE, projection_matrix.data());
	gl.BindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, position)));
	gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, texcoord)));

	for (size_t i = 0; i < texture_ids.size(); ++i) {
		CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, texture_ids[i]));
		CHECK_RENDER_ERROR(glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, reinterpret_cast<void *>(i * 6 * sizeof(GLuint))));
	}

	gl.DisableVertexAttribArray(0);
	gl.DisableVertexAttribArray(1);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
	gl.UseProgram(0);
}
