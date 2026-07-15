// Copyright (c) 2026
// All rights reserved.

#include "skia_runtime/skia_gpu_context_host.h"

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#endif

bool SkiaGpuContextHost::EnsureCurrentContext() {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (context)
		return true;

	gl_interface = GrGLMakeNativeInterface();
	if (!gl_interface)
		return false;

	context = GrDirectContexts::MakeGL(gl_interface);
	return static_cast<bool>(context);
#else
	return false;
#endif
}

GrDirectContext *SkiaGpuContextHost::Get() const {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	return context.get();
#else
	return nullptr;
#endif
}

void SkiaGpuContextHost::SyncExternalState() {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (context)
		context->resetContext();
#endif
}

void SkiaGpuContextHost::ResetTextureBindingsForExternalUse() {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (context)
		context->resetGLTextureBindings();
#endif
}

void SkiaGpuContextHost::FlushAndSubmit() {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (context)
		context->flushAndSubmit();
#endif
}

void SkiaGpuContextHost::Reset() {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (context) {
		context->releaseResourcesAndAbandonContext();
		context.reset();
	}
	gl_interface.reset();
#endif
}
