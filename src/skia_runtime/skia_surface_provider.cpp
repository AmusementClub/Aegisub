// Copyright (c) 2026
// All rights reserved.

#include "skia_runtime/skia_surface_provider.h"

#ifdef AEGISUB_WITH_SKIA_GPU_GL
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <include/core/SkColorSpace.h>
#include <include/core/SkSurfaceProps.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#endif

#include <algorithm>

sk_sp<SkSurface> SkiaSurfaceProvider::AcquireFramebufferSurface(
	GrDirectContext *context,
	SkiaFramebufferSurfaceDescriptor const& descriptor) const {
#ifdef AEGISUB_WITH_SKIA_GPU_GL
	if (!context || descriptor.width <= 0 || descriptor.height <= 0)
		return nullptr;

	GrGLFramebufferInfo framebuffer_info;
	framebuffer_info.fFBOID = descriptor.framebuffer_id;
	framebuffer_info.fFormat = GL_RGBA8;

	auto const render_target = GrBackendRenderTargets::MakeGL(
		descriptor.width,
		descriptor.height,
		std::max(0, descriptor.sample_count),
		std::max(0, descriptor.stencil_bits),
		framebuffer_info);

	GrSurfaceOrigin const origin = descriptor.bottom_left_origin
		? kBottomLeft_GrSurfaceOrigin
		: kTopLeft_GrSurfaceOrigin;
	SkSurfaceProps const props(0, kUnknown_SkPixelGeometry);
	return SkSurfaces::WrapBackendRenderTarget(
		context,
		render_target,
		origin,
		kRGBA_8888_SkColorType,
		SkColorSpace::MakeSRGB(),
		&props);
#else
	(void)context;
	(void)descriptor;
	return nullptr;
#endif
}
