// Copyright (c) 2026

#include "csri_renderer_discovery.h"

#include "native_library.h"

namespace csri {

std::vector<std::string> EnumerateRendererLibraryFiles() {
	return agi::native::EnumerateLibrariesInExecutableRelativeDirectory("csri");
}

std::vector<std::string> EnumerateRendererLibraryFiles(std::string_view executable_directory) {
	return agi::native::EnumerateLibrariesInExecutableRelativeDirectory("csri", executable_directory);
}

}
