// Copyright (c) 2026
// All rights reserved.

#pragma once

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
#include <include/core/SkRefCnt.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#endif

class SkiaGpuContextHost {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	sk_sp<const GrGLInterface> gl_interface;
	sk_sp<GrDirectContext> context;
#endif

public:
	bool EnsureCurrentContext();
	GrDirectContext *Get() const;
	void SyncExternalState();
	void ResetTextureBindingsForExternalUse();
	void FlushAndSubmit();
	void Reset();
};
