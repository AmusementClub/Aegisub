#include "skia_video_overlay_gl.h"

#include "../legacy_gl_draw.h"

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace {
void DrawBoundedTexturedQuad(unsigned int texture, SkiaOverlayDeviceBounds const& bounds) {
	GLfloat const tex_coords[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		0.0f, 1.0f,
	};
	GLfloat const vertices[] = {
		static_cast<GLfloat>(bounds.x), static_cast<GLfloat>(bounds.y),
		static_cast<GLfloat>(bounds.x + bounds.width), static_cast<GLfloat>(bounds.y),
		static_cast<GLfloat>(bounds.x + bounds.width), static_cast<GLfloat>(bounds.y + bounds.height),
		static_cast<GLfloat>(bounds.x), static_cast<GLfloat>(bounds.y + bounds.height),
	};

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

void CompositeSkiaVideoOverlayTextures(
	unsigned int normal_texture,
	bool draw_normal,
	SkiaOverlayDeviceBounds const& normal_bounds,
	unsigned int invert_texture,
	bool draw_invert,
	SkiaOverlayDeviceBounds const& invert_bounds,
	int canvas_width,
	int canvas_height) {
	if (canvas_width <= 0 || canvas_height <= 0)
		return;

	legacy_gl::ResetCompatibilityState();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, canvas_width, canvas_height, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisableClientState(GL_COLOR_ARRAY);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	if (draw_normal && normal_texture && !normal_bounds.IsEmpty()) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		DrawBoundedTexturedQuad(normal_texture, normal_bounds);
		glDisable(GL_BLEND);
	}

	if (draw_invert && invert_texture && !invert_bounds.IsEmpty()) {
		glDisable(GL_BLEND);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.0f);
		glEnable(GL_COLOR_LOGIC_OP);
		glLogicOp(GL_INVERT);
		DrawBoundedTexturedQuad(invert_texture, invert_bounds);
		glDisable(GL_COLOR_LOGIC_OP);
		glDisable(GL_ALPHA_TEST);
	}
}
