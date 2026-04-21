// Copyright (c) 2026
// All rights reserved.

#pragma once

#ifdef WITH_SKIA
#include <include/core/SkRefCnt.h>
#endif

class GrDirectContext;
class SkSurface;

struct SkiaFramebufferSurfaceDescriptor {
	int width = 0;
	int height = 0;
	int sample_count = 0;
	int stencil_bits = 0;
	unsigned int framebuffer_id = 0;
	bool bottom_left_origin = true;
};

class SkiaSurfaceProvider {
public:
	sk_sp<SkSurface> AcquireFramebufferSurface(
		GrDirectContext *context,
		SkiaFramebufferSurfaceDescriptor const& descriptor) const;
};
