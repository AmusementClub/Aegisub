// Copyright (c) 2026
// All rights reserved.

#pragma once

#ifdef WITH_SKIA
#include <include/core/SkRefCnt.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#endif

class SkiaGpuContextHost {
#ifdef WITH_SKIA
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
