// Copyright (c) 2026

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace csri {

std::vector<std::string> EnumerateRendererLibraryFiles();
std::vector<std::string> EnumerateRendererLibraryFiles(std::string_view executable_directory);

}
