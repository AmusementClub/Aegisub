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

#include "video_render_opengl_proc_loader.h"

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

namespace legacy_gl {

template <typename Proc>
inline Proc LoadOptionalProc(char const *name, char const *fallback_name = nullptr) {
	if (auto *proc = opengl::GetProcAddress(name))
		return reinterpret_cast<Proc>(proc);
	if (fallback_name) {
		if (auto *proc = opengl::GetProcAddress(fallback_name))
			return reinterpret_cast<Proc>(proc);
	}
	return nullptr;
}

struct CompatibilityFunctions {
	PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;
	PFNGLCLIENTACTIVETEXTUREPROC ClientActiveTexture = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
};

inline CompatibilityFunctions const& GetCompatibilityFunctions() {
	static const CompatibilityFunctions functions = {
		LoadOptionalProc<PFNGLACTIVETEXTUREPROC>("glActiveTexture"),
		LoadOptionalProc<PFNGLCLIENTACTIVETEXTUREPROC>("glClientActiveTexture"),
		LoadOptionalProc<PFNGLBINDBUFFERPROC>("glBindBuffer"),
		LoadOptionalProc<PFNGLUSEPROGRAMPROC>("glUseProgram"),
		LoadOptionalProc<PFNGLBINDVERTEXARRAYPROC>("glBindVertexArray"),
	};
	return functions;
}

inline void ResetCompatibilityState() {
	auto const& gl = GetCompatibilityFunctions();
	if (gl.UseProgram)
		gl.UseProgram(0);
	if (gl.BindVertexArray)
		gl.BindVertexArray(0);
	if (gl.BindBuffer) {
		gl.BindBuffer(GL_ARRAY_BUFFER, 0);
		gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	if (gl.ActiveTexture)
		gl.ActiveTexture(GL_TEXTURE0);
	if (gl.ClientActiveTexture)
		gl.ClientActiveTexture(GL_TEXTURE0);
}

inline void SetupBottomLeftOrtho(int canvas_width, int canvas_height) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, canvas_width, 0.0, canvas_height, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

inline void DrawTexturedQuad(GLuint texture, int canvas_width, int canvas_height) {
	ResetCompatibilityState();
	SetupBottomLeftOrtho(canvas_width, canvas_height);

	GLfloat const tex_coords[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		0.0f, 1.0f
	};
	GLfloat const vertices[] = {
		0.0f, 0.0f,
		static_cast<GLfloat>(canvas_width), 0.0f,
		static_cast<GLfloat>(canvas_width), static_cast<GLfloat>(canvas_height),
		0.0f, static_cast<GLfloat>(canvas_height)
	};

	glDisableClientState(GL_COLOR_ARRAY);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glTexCoordPointer(2, GL_FLOAT, 0, tex_coords);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
}

}
