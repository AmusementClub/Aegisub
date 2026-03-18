#ifdef WITH_LIBPLACEBO

#include "../../src/video_renderer_placebo_runtime.h"

#include <iostream>

int main() {
	if (!placebo::runtime::IsAvailable()) {
		std::cerr << "libplacebo runtime unavailable: " << placebo::runtime::GetLoadError() << std::endl;
		return 1;
	}

	auto const& api = placebo::runtime::GetApi();
	if (!api.log_create || !api.opengl_create || !api.renderer_create || !api.opengl_wrap || !api.upload_plane) {
		std::cerr << "libplacebo runtime loaded but required symbols are missing." << std::endl;
		return 2;
	}

	std::cout
		<< "loaded_library=" << placebo::runtime::GetLoadedLibrary() << "\n"
		<< "loaded_version=" << placebo::runtime::GetLoadedVersion() << "\n"
		<< "loaded_fix_version=" << placebo::runtime::GetLoadedFixVersion() << std::endl;
	return 0;
}

#else

int main() {
	return 0;
}

#endif
