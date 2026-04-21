// Copyright (c) 2026
// All rights reserved.

#include "skia_runtime/skia_gpu_context_host.h"

#ifdef WITH_SKIA
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#endif

bool SkiaGpuContextHost::EnsureCurrentContext() {
#ifdef WITH_SKIA
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
#ifdef WITH_SKIA
	return context.get();
#else
	return nullptr;
#endif
}

void SkiaGpuContextHost::SyncExternalState() {
#ifdef WITH_SKIA
	if (context)
		context->resetContext();
#endif
}

void SkiaGpuContextHost::ResetTextureBindingsForExternalUse() {
#ifdef WITH_SKIA
	if (context)
		context->resetGLTextureBindings();
#endif
}

void SkiaGpuContextHost::FlushAndSubmit() {
#ifdef WITH_SKIA
	if (context)
		context->flushAndSubmit();
#endif
}

void SkiaGpuContextHost::Reset() {
#ifdef WITH_SKIA
	if (context) {
		context->releaseResourcesAndAbandonContext();
		context.reset();
	}
	gl_interface.reset();
#endif
}
