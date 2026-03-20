// Copyright (c) 2026

#include "csri_renderer_discovery.h"
#include "native_library.h"

#include "../libaegisub/include/libaegisub/exception.h"

extern "C" {
#include "../vendor/csri/lib/csrilib.h"
#include "../vendor/csri/include/subhelp.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif
}

#include <cstring>
#include <memory>
#include <string>

#ifdef GetMessage
#undef GetMessage
#endif

namespace {
using RendererInfoFunction = csri_info *(*)(csri_rend *);
using RendererDefaultFunction = csri_rend *(*)();
using RendererNextFunction = csri_rend *(*)(csri_rend *);

void AddRenderer(csri_rend *renderer, csri_wrap_rend const& prototype, csri_info *info) {
	auto *wrapped = static_cast<csri_wrap_rend *>(malloc(sizeof(csri_wrap_rend)));
	if (!wrapped)
		return;

	std::memcpy(wrapped, &prototype, sizeof(csri_wrap_rend));
	wrapped->rend = renderer;
	wrapped->info = info;
	csrilib_rend_initadd(wrapped);
}

void LoadRendererLibrary(std::string const& path) {
	try {
		agi::native::Library library = agi::native::Library::Load(path);
		if (library.TryResolveSymbol<void *(*)()>("csri_library")) {
			subhelp_log(CSRI_LOG_WARNING, "ignoring library %s", path.c_str());
			return;
		}

		csri_wrap_rend prototype = { };
		prototype.query_ext = library.ResolveSymbol<decltype(prototype.query_ext)>("csri_query_ext");
		subhelp_logging_pass(static_cast<csri_logging_ext *>(prototype.query_ext(nullptr, CSRI_EXT_LOGGING)));
		prototype.open_file = library.ResolveSymbol<decltype(prototype.open_file)>("csri_open_file");
		prototype.open_mem = library.ResolveSymbol<decltype(prototype.open_mem)>("csri_open_mem");
		prototype.close = library.ResolveSymbol<decltype(prototype.close)>("csri_close");
		prototype.request_fmt = library.ResolveSymbol<decltype(prototype.request_fmt)>("csri_request_fmt");
		prototype.render = library.ResolveSymbol<decltype(prototype.render)>("csri_render");

		auto renderer_info = library.ResolveSymbol<RendererInfoFunction>("csri_renderer_info");
		auto renderer_default = library.ResolveSymbol<RendererDefaultFunction>("csri_renderer_default");
		auto renderer_next = library.ResolveSymbol<RendererNextFunction>("csri_renderer_next");

#ifdef _WIN32
		prototype.os.dlhandle = reinterpret_cast<HMODULE>(library.ReleaseHandle());
#else
		prototype.os.dlhandle = library.ReleaseHandle();
		struct stat st = { };
		if (stat(path.c_str(), &st) == 0) {
			prototype.os.device = st.st_dev;
			prototype.os.inode = st.st_ino;
		}
#endif

		subhelp_log(CSRI_LOG_INFO, "loading %s", path.c_str());
		for (auto *renderer = renderer_default(); renderer; renderer = renderer_next(renderer))
			AddRenderer(renderer, prototype, renderer_info(renderer));
	}
	catch (agi::EnvironmentError const& err) {
		subhelp_log(CSRI_LOG_WARNING, "%s", err.GetMessage().c_str());
	}
}
}

extern "C" void csrilib_os_init() {
	for (auto const& path : csri::EnumerateRendererLibraryFiles())
		LoadRendererLibrary(path);
}
